/* Copyright 2023 Simon Willcocks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CK_types.h"
#include "Processor/processor.h"
#include "raw_memory_manager.h"

// Short descriptors implementation

typedef struct {
  uint32_t type2:2;
  uint32_t B:1;
  uint32_t C:1;
  uint32_t XN:1;
  uint32_t Domain:4;
  uint32_t P:1;
  uint32_t AF:1;
  uint32_t unprivileged_access:1;
  uint32_t TEX:3;
  uint32_t read_only:1;
  uint32_t S:1;
  uint32_t nG:1;
  uint32_t supersection:1;
  uint32_t not_secure:1;
  uint32_t base:12;
} l1tt_section_entry;

typedef struct {
  uint32_t type1:2;
  uint32_t SBZ1:1;
  uint32_t NS:1;
  uint32_t SBZ2:1;
  uint32_t Domain:4;
  uint32_t P:1;
  uint32_t page_table_base:22;
} l1tt_table_entry;

typedef union l1tt_entry {
  uint32_t raw;
  uint32_t type:2; // 0 = handler, 1 = Page table, 2 = Section (or supersection), executable, 3 = S PXN
  l1tt_table_entry table;
  l1tt_section_entry section;
  memory_fault_handler handler;
} l1tt_entry;

// AP[2:1] access permissions model
//
typedef union {
  struct {
    uint32_t XN:1; // If small_page == 1, else must be 1 for large page or 0 for no memory
    uint32_t small_page:1;
    uint32_t B:1;
    uint32_t C:1;
    uint32_t AF:1;
    uint32_t unprivileged_access:1;
    uint32_t TEX:3;
    uint32_t read_only:1;
    uint32_t S:1;
    uint32_t nG:1;
    uint32_t page_base:20;
  };
  uint32_t raw;
  uint32_t type:2; // 0 = handler, 1 = large page, 2 = small executable page, 3 = small data page
  memory_fault_handler handler;
} l2tt_entry;

union l2tt {
  l2tt_entry entry[256];
  struct { // only used when unused
    l2tt *next;
    l2tt *prev;
  };
};

typedef struct {
  l1tt_entry entry[4096];
} l1tt;

#include "mpsafe_dll.h"
MPSAFE_DLL_TYPE( l2tt );

typedef union {
  void *rawp;
  uint32_t raw;
  struct {
    uint32_t offset:12;
    uint32_t page:8;
    uint32_t section:12;
  };
  struct {
    uint32_t page_offset:12;
    uint32_t page_base:20;
  };
  struct {
    uint32_t section_offset:20;
  };
} arm32_ptr;

static inline uint32_t fault_address()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[dfar], c6, c0, 0" : [dfar] "=r" (result ) );
  return result;
}

static inline uint32_t data_fault_type()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[dfsr], c5, c0, 0" : [dfsr] "=r" (result ) );
  return result;
}

static inline uint32_t instruction_fault_type()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[ifsr], c5, c0, 1" : [ifsr] "=r" (result ) );
  return result;
}

// Defined at link time:
extern l1tt translation_table;
extern l1tt global_translation_table;
extern l2tt local_kernel_page_table[4];
extern l2tt global_kernel_page_tables[4];
extern uint8_t top_of_boot_RAM;

static inline l2tt *mapped_table( l1tt_table_entry entry )
{
  arm32_ptr base = { .page_base = shared.mmu.l2tables_phys_base };

  arm32_ptr phys = { .raw = entry.page_table_base << 10 };

  if (phys.section != base.section) {
    uint32_t index = entry.page_table_base & 3;
    return &local_kernel_page_table[index];
  }
  else {
    extern l2tt VMSAv6_Level2_Tables[4096];
    uint32_t index = entry.page_table_base & (4096-1);
    return &VMSAv6_Level2_Tables[index];
  }
}

static inline l1tt_table_entry table_entry( l2tt *table )
{
  arm32_ptr tab = { .rawp = table };
  arm32_ptr base = { .page_base = shared.mmu.l2tables_phys_base };

  if (0 != (tab.raw & 0x3ff)) PANIC;

  uint32_t offset = 0x000ffc00 & tab.raw;

  l1tt_table_entry entry = { .type1 = 1,
                .page_table_base = base.raw | offset };

  return entry;
}

static inline l2tt *make_section_page_mappable( arm32_ptr virt )
{
  // Fill a level 2 table with the same handler
  // Reminder: MMU structures are protected by shared.mmu.lock
  if (0 == shared.mmu.free) PANIC;

  l2tt *table = shared.mmu.free->next;

  dll_detach_l2tt( table );

  translation_table.entry[virt.section].table = table_entry( table );

  return table;
}

void clear_memory_region(
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler )
{
  if (va_pages == 0) PANIC;

  arm32_ptr virt = { .raw = va_base };
  arm32_ptr last = { .raw = va_base + va_pages - 1 };

  l2tt *l2table = 0;

  if (0 != virt.section_offset) {
    // Memory block starts part way through a section.

    if (translation_table.entry[virt.section].type == 1) {
      l2table = mapped_table( translation_table.entry[virt.section].table );
    }
    else if (translation_table.entry[virt.section].type == 0) {
      memory_fault_handler handler =
                translation_table.entry[virt.section].handler;

      l2table = make_section_page_mappable( virt );

      // Copy the section handler to the remaining entries
      for (int i = 0; i < virt.page; i++) {
        l2table->entry[i].handler = handler;
      }

      if (virt.section == last.section) {
        for (int i = last.page + 1; i < 256; i++) {
          l2table->entry[i].handler = handler;
        }
      }
    }

    if (l2table == 0) PANIC;

    for (; va_pages > 0 && 0 != virt.section_offset; va_pages--) {
      l2table->entry[virt.page++].handler = handler;
    }
  }

  if (0 != virt.section_offset) PANIC;

  while (va_pages > 256) { // Sections
    if (translation_table.entry[virt.section].type == 1) {
      // Free up table
    }
    translation_table.entry[virt.section++].handler = handler;
    va_pages -= 256;
  }

  if (va_pages > 0) {
    // Memory block ends part way through a section.
    if (translation_table.entry[virt.section].type == 1) {
      l2table = mapped_table( translation_table.entry[virt.section].table );
    }
    else if (translation_table.entry[virt.section].type == 0) {
      memory_fault_handler handler =
                translation_table.entry[virt.section].handler;

      l2table = make_section_page_mappable( virt );

      // Copy the section handler to the remaining entries
      for (int i = va_pages; i < 256; i++) {
        l2table->entry[i].handler = handler;
      }
    }

    if (l2table == 0) PANIC;

    for (; va_pages > 0; va_pages--) {
      l2table->entry[virt.page++].handler = handler;
    }
  }
}

// Orthogonal features. Cacheing, permissions. Pages default to small; large
// pages are always data only (not executable)

static l1tt_entry const cached_section = { .section = { .type2 = 2, .TEX = 5, .C = 0, .B = 1 } };
static l2tt_entry const cached_page = { .TEX = 5, .C = 0, .B = 1 };

static l1tt_entry const rwx_section = { .section = { .type2 = 2, .XN = 0, .read_only = 0 } };
static l2tt_entry const rwx_page = { .small_page = 1, .XN = 0, .read_only = 0 };
static l1tt_entry const rw_section = { .section = { .type2 = 2, .XN = 1, .read_only = 0 } };
static l2tt_entry const rw_page = { .small_page = 1, .XN = 1, .read_only = 0 };
static l1tt_entry const rx_section = { .section = { .type2 = 2, .XN = 0, .read_only = 1 } };
static l2tt_entry const rx_page = { .small_page = 1, .XN = 0, .read_only = 1 };
static l1tt_entry const r_section = { .section = { .type2 = 2, .XN = 1, .read_only = 1 } };
static l2tt_entry const r_page = { .small_page = 1, .XN = 1, .read_only = 1 };

// Device-nGnRnE, uncached
static l1tt_entry const dev_section = { .section = { .type2 = 2, .XN = 1, .read_only = 0, .TEX = 0, .B = 0, .C = 0 } };
static l2tt_entry const dev_page = { .small_page = 1, .XN = 1, .read_only = 0, .TEX = 0, .B = 0, .C = 0 };

void map_memory( memory_mapping const *mapping )
{
  if (mapping->pages == 0) PANIC;

  bool reclaimed = core_claim_lock( &shared.mmu.lock, workspace.core + 1 );

  // Note: Could allow for base_page to be above 4GiB,
  // the extra bits go in the supersection entry, which
  // is the only way to access them in small tables.

  // Could be useful for caches or dynamic areas, at least.

  // To allow for unusual mappings, e.g. six pages in one section,
  // 20 more in the next, this routine may recurse.

  arm32_ptr phys = { .raw = (mapping->base_page << 12) };
  arm32_ptr virt = { .raw = mapping->va };

  // First off, are we mapping pages, large pages, sections,
  // or supersections (or a mixture)?

  // Initially, only pages and sections, can be expanded later.

  if (phys.section_offset == 0
   && virt.section_offset == 0
   && (mapping->pages & 0xff) == 0) {
    // Sections, at least, no need to split
    l1tt_entry l1;

    switch (mapping->type) {
    case CK_MemoryRWX: l1 = rwx_section; break;
    case CK_MemoryRW: l1 = rw_section; break;
    case CK_MemoryRX: l1 = rx_section; break;
    case CK_MemoryR: l1 = r_section; break;
    case CK_Device: l1 = dev_section; break;
    default:
      PANIC;
    }

    uint32_t sections = mapping->pages >> 8;

    l1tt_entry entry = { .raw = l1.raw | cached_section.raw };

    if (mapping->all_cores)
      entry.section.S = 1;

    if (mapping->usr32_access)
      entry.section.unprivileged_access = 1;

    if (mapping->map_specific)
      entry.section.nG = 1;

    entry.section.AF = 1;

    entry.section.base = phys.section;
    for (int i = 0; i < sections; i++) {
      translation_table.entry[virt.section + i] = entry;
      if (mapping->all_cores)
        global_translation_table.entry[virt.section + i] = entry;
      entry.section.base++;
    }
  }
  else {
    // Pages, for sure
    // Does the mapping include something bigger?
    // Are all the pages in the same section?
    // Not yet implemented!
    extern l2tt VMSAv6_Level2_Tables[4096];

    l2tt *table = 0;

    if (translation_table.entry[virt.section].type == 1) {
      table = mapped_table( translation_table.entry[virt.section].table );
    }
    else if (translation_table.entry[virt.section].type == 0) {
      memory_fault_handler handler =
                translation_table.entry[virt.section].handler;

      table = make_section_page_mappable( virt );

      for (int i = 0; i < 256; i++) {
        table->entry[i].handler = handler;
      }
    }

    if (translation_table.entry[virt.section].type != 1) PANIC;
    if (table == 0) PANIC;

    l2tt_entry entry;

    switch (mapping->type) {
    case CK_MemoryRWX: entry = rwx_page; break;
    case CK_MemoryRW: entry = rw_page; break;
    case CK_MemoryRX: entry = rx_page; break;
    case CK_MemoryR: entry = r_page; break;
    case CK_Device: entry = dev_page; break;
    default:
      PANIC;
    }

    if (mapping->all_cores)
      entry.S = 1;

    if (mapping->usr32_access)
      entry.unprivileged_access = 1;

    if (mapping->map_specific)
      entry.nG = 1;

    entry.AF = 1;
    entry.page_base = phys.page_base;

    for (int i = 0; i < mapping->pages; i++) {
      local_kernel_page_table[0].entry[virt.page] = entry;
      if (mapping->all_cores)
        global_kernel_page_tables[0].entry[virt.page] = entry;

      entry.page_base++;
    }
  }

  // I misunderstood the meaning of the top bits in a supersection; all
  // entries that are for a supersection must have the same bits 20-31,
  // bits 20-23 are used for the physical address bits above the normal
  // 32-bit address space. G4.4

  // The MMU has been configured to use the cache, so the writes don't
  // have to be flushed to RAM.
  push_writes_to_cache();

  if (!reclaimed) core_release_lock( &shared.mmu.lock );
}

// These routines can update the tables directly, others have to call
// map_memory.
bool check_global_table( uint32_t va, uint32_t fault )
{
  arm32_ptr virt = { .raw = va };

  switch (fault & 0xf) {
  case 5: // Translation fault, level 1
    {
      l1tt_entry l1 = global_translation_table.entry[virt.section];

      translation_table.entry[virt.section] = l1;

      if (l1.type == 0) {
        if (check_global_table == l1.handler)
          return false;
        else
          return l1.handler( va, fault );
      }
      return true;
    }
    break;
  case 7: // Translation fault, level 2
    {
      l1tt_table_entry l1 = translation_table.entry[virt.section].table;
      l2tt *l2table = mapped_table( l1 );

      l2tt_entry l2;

      if (l2table == &local_kernel_page_table[0]) {
        l2 = global_kernel_page_tables[0].entry[virt.page];
      }
      else {
        PANIC; // Untested
        l2 = l2table->entry[virt.page];
      }

      if (l2.type == 0) {
        if (check_global_table == l2.handler)
          return false;
        else
          return l2.handler( va, fault );
      }

      l2table->entry[virt.page] = l2;
      push_writes_to_cache();

      return true;
    }
    break;
  default: PANIC;
  }

  return false;
}

void create_default_translation_tables( uint32_t memory )
{
  l1tt_entry *tt = (void*) memory;
  l2tt_entry *pages = (void *) (&tt[4096]);

  for (int i = 0; i < 4096; i++) {
    tt[i].handler = check_global_table;
  }

  for (int i = 0; i < 256; i++) {
    pages[i].handler = check_global_table;
  }

  // Map the low physical memory to VA 0, RWX
  {
    l1tt_entry supersection = { .section = {
      .type2 = 2,
      .B = 1,
      .C = 0,
      .XN = 0,
      .Domain = 0,
      .P = 0,
      .AF = 1,
      .unprivileged_access = 1,
      .TEX = 5,
      .read_only = 0,
      .S = 1,
      .nG = 1,
      .supersection = 1,
      .not_secure = 0
    } };

    uint8_t *ram_top = &top_of_boot_RAM;
    uint32_t sections = ((uint32_t) ram_top) >> 20;

    l1tt_entry entry = { .raw = supersection.raw };
    int i = 0;
    while (i + 16 <= sections) {
      for (int s = 0; s < 16; s++) {
        tt[i++] = entry;
      }
      entry.section.base += 16;
    }
    entry.section.supersection = 0;
    while (i < sections) {
      tt[i++] = entry;
      entry.section.base ++;
    }
  }

  // Map the OS image at its virtual address
  {
    extern uint8_t __end__;
    extern uint8_t _start;
    arm32_ptr high_memory = { .rawp = &_start };
    uint32_t img_size = &__end__ - &_start;

    uint32_t sections = (img_size + (mmu_section_size - 1)) >> 20;

    if (0 != high_memory.section_offset) PANIC;

    l1tt_entry entry = {
      .section = { .type2 = 2, .XN = 0, .read_only = 1,
                    .unprivileged_access = 1, .TEX = 5,
                    .C = 0, .B = 1, .AF = 1 } };

    for (int i = 0; i < sections; i++) {
      tt[high_memory.section + i] = entry;
      entry.section.base++;
    }
  }

  // Install the special case translation table for the top MiB
  {
    l1tt_entry entry = { .raw = (uint32_t) pages };
    entry.table.type1 = 1;
    entry.table.SBZ1 = 0;
    entry.table.SBZ2 = 0;
    entry.table.Domain = 0;
    entry.table.NS = 0;
    entry.table.P = 0;
    tt[0xfff] = entry;
  }

  // ... and give access to these tables
  // 0x157 = cached global small page with AF set, RW-

  uint32_t core0_workspace = (uint32_t) &top_of_boot_RAM;
  core0_workspace -= CORE_WORKSPACE;

  {
    arm32_ptr p = { .rawp = &translation_table };
    l2tt_entry entry = { .raw = 0x157 | memory };
    pages[p.page+0] = entry; entry.page_base++;
    pages[p.page+1] = entry; entry.page_base++;
    pages[p.page+2] = entry; entry.page_base++;
    pages[p.page+3] = entry;
  }
  {
    arm32_ptr p = { .rawp = &global_translation_table };
    // Shared, RW
    l2tt_entry entry = { .raw = 0x557 | core0_workspace };
    pages[p.page+0] = entry; entry.page_base++;
    pages[p.page+1] = entry; entry.page_base++;
    pages[p.page+2] = entry; entry.page_base++;
    pages[p.page+3] = entry;
  }
  {
    arm32_ptr p = { .rawp = &local_kernel_page_table[0] };
    uint32_t table = (uint32_t) pages;
    l2tt_entry entry = { .raw = 0x157 | table };
    pages[p.page+0] = entry;
  }
  {
    arm32_ptr p = { .rawp = &global_kernel_page_tables };
    l2tt_entry entry = { .raw = 0x557 | (core0_workspace + 0x4000) };
    pages[p.page+0] = entry;
  }

  {
    extern uint8_t _start;
    extern uint8_t __end__;
    uint32_t free = &__end__ - &_start;
    free = free + mmu_section_size;
    free = free & ~(mmu_section_size-1);
    arm32_ptr p = { .rawp = &shared };
    l2tt_entry entry = { .raw = 0x557 | free };
    uint32_t count = P( sizeof( shared_workspace ) );
    for (int i = 0; i < count; i++) {
      pages[p.page+i] = entry;
      entry.page_base ++;
    }
  }

  {
    arm32_ptr p = { .rawp = &workspace };
    l2tt_entry entry = { .raw = 0x157 | (0x5000 + memory) };
    uint32_t count = P( sizeof( core_workspace ) );
    for (int i = 0; i < count; i++) {
      pages[p.page+i] = entry;
      entry.page_base ++;
    }
  }

  push_writes_to_cache();
}

void forget_boot_low_memory_mapping()
{
  uint8_t *ram_top = &top_of_boot_RAM;
  uint32_t sections = ((uint32_t) ram_top) >> 20;

  for (int i = 0; i < sections; i++) {
    translation_table.entry[i].handler = check_global_table;
  }

  push_writes_to_cache();

  // Clear any TLB using ASID 1
  asm ( "mcr p15, 0, %[one], c8, c7, 2" : : [one] "r" (1) );

  mmu_switch_map( 0 );

  push_writes_to_cache();
}

void enable_page_level_mapping()
{
  // This is probably an extreme number of tables, TODO see how 
  // many we actually need and adjust the algorithms accordingly.
  // If 1 MiB more or less becomes a problem!

  extern l2tt VMSAv6_Level2_Tables[4096];

  shared.mmu.l2tables_phys_base = claim_contiguous_memory( 0x100 ); // 1 MiB
  memory_mapping l2tts = {
    .base_page = shared.mmu.l2tables_phys_base,
    .pages = 0x100,
    .vap = &VMSAv6_Level2_Tables,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (l2tts.base_page == 0xffffffff) PANIC;
  map_memory( &l2tts );
  // for (int i = 0; i < number_of( VMSAv6_Level2_Tables ); i++) {
  for (int i = 0; i < 64; i++) {
    l2tt *t = &VMSAv6_Level2_Tables[i];
    dll_new_l2tt( t );
    dll_attach_l2tt( t, &shared.mmu.free );
  }
}

static memory_fault_handler find_handler( uint32_t fa )
{
  arm32_ptr va = { .raw = fa };
  switch (translation_table.entry[va.section].type) {
  case 0:
    return translation_table.entry[va.section].handler;
  case 1:
    {
      l2tt *table = mapped_table( translation_table.entry[va.section].table );
      l2tt_entry l2 = table->entry[va.page];
      if (l2.type != 0) PANIC;
      return l2.handler;
    }
  default: PANIC;
  }
  return 0;
}

void __attribute__(( naked )) prefetch_handler()
{
  PANIC;
  
}

static bool __attribute__(( noinline )) handle_data_abort()
{
  uint32_t fa = fault_address();
  uint32_t ft = data_fault_type();

  // Real hardware appears to fill in a value for Domain which may not be
  // zero. Oh, I wonder if it simply copies the appropriate bits from the
  // table entry, regardless of if the entry is invalid?

  // Domain errors should never happen, and when they do should be
  // handled at this level. The fault type with Domain errors will
  // not be 5 or 7.
  if ((ft & ~0x8f0) != 7
   && (ft & ~0x8f0) != 5) {
    return false;
  }

  memory_fault_handler handler = find_handler( fa );

  if (handler == 0) PANIC; // Probably report it to the application

  return handler( fa, ft );
}


void __attribute__(( naked )) data_abort_handler()
{
  asm volatile (
        "  sub lr, lr, #8"
      "\n  srsdb sp!, #0x17 // Store return address and SPSR"
      "\n  push { "C_CLOBBERED" }"
      );

  if (!handle_data_abort()) PANIC;

  asm volatile ( "pop { "C_CLOBBERED" }"
    "\n  rfeia sp! // Restore execution and SPSR" );

  __builtin_unreachable();
}

void mmu_switch_map( uint32_t new_map )
{
  asm ( "mcr p15, 0, %[map], c13, c0, 1" : : [map] "r" (new_map) );
}

void forget_current_map()
{
  uint32_t map;
  asm ( "mrc p15, 0, %[map], c13, c0, 1" : [map] "=r" (map) );
  asm ( "mcr p15, 0, %[map], c8, c7, 2" : : [map] "r" (map) );
}

