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

extern void send_number( uint32_t n, char c );

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
    // uint32_t section:12; above
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

static inline l2tt *shared_table( l1tt_table_entry entry )
{
  arm32_ptr phys = { .raw = entry.page_table_base << 10 };

  extern l2tt VMSAv6_Level2_Tables[4096];
  arm32_ptr vbase = { .rawp = &VMSAv6_Level2_Tables };
  vbase.section_offset = phys.section_offset;

  return vbase.rawp;
}

static inline l2tt *mapped_global_table( l1tt_table_entry entry )
{
  arm32_ptr base = { .page_base = shared.mmu.l2tables_phys_base };

  arm32_ptr phys = { .raw = entry.page_table_base << 10 };

  if (phys.section != base.section) {
    uint32_t index = entry.page_table_base & 3;
    if (index != 0) PANIC; // Untested, do they always match up with locals?
    return &global_kernel_page_tables[index];
  }
  else {
    return shared_table( entry );
  }
}

static inline l2tt *mapped_table( l1tt_table_entry entry )
{
  arm32_ptr base = { .page_base = shared.mmu.l2tables_phys_base };

  arm32_ptr phys = { .raw = entry.page_table_base << 10 };

  if (phys.section != base.section) {
    uint32_t index = entry.page_table_base & 3;
    if (index != 0) PANIC; // Untested, do they always match up with globals?
    return &local_kernel_page_table[index];
  }
  else {
    return shared_table( entry );
  }
}

static inline l1tt_table_entry table_entry( l2tt *table )
{
  arm32_ptr tab = { .rawp = table };
  arm32_ptr base = { .page_base = shared.mmu.l2tables_phys_base };

  tab.section = base.section;

  if (0 != (tab.section_offset & 0x3ff)) PANIC;

  l1tt_table_entry entry = { .type1 = 1, .page_table_base = tab.raw >> 10 };

  return entry;
}

static inline l2tt *get_free_table()
{
  bool reclaimed = core_claim_lock( &shared.mmu.lock, workspace.core + 1 );

  // Replace an invalid l1tt entry with a table so that pages can
  // be mapped into the area.

  l2tt *table;

  if (0 == shared.mmu.free) {
    // Early in the boot process, don't panic yet!
    // OK, do panic; the current code never encounters this problem
    // and may never do so.
    // If it does, find a way to allocate spare space in the core's
    // workspace to a 1024-byte table.
    PANIC;
  }
  else {
    // Last one...?
    if (shared.mmu.free->next == shared.mmu.free) PANIC;

    table = shared.mmu.free->next;

    dll_detach_l2tt( table );
  }

  if (!reclaimed) core_release_lock( &shared.mmu.lock );

  return table;
}

void clear_memory_region(
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler )
{
  // Only affecting the local tables, with interrupts disabled.

  // Any writes to currently mapped memory...
  push_writes_to_cache();

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

      l2table = get_free_table();

      translation_table.entry[virt.section].table = table_entry( l2table );

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

  // The only multi-processing danger is when level 2 tables are
  // released. We'll make a collection of them and release them
  // to the shared pool all at once.
  // Alternatively, each core could maintain a pool of its own.

  l2tt *freed = 0;

  while (va_pages >= 256) { // Sections
    l1tt_entry l1 = translation_table.entry[virt.section];
    if (l1.type == 1) {
      // Free up table

      l2tt *l2 = mapped_table( l1.table );
      dll_new_l2tt( l2 );
      dll_attach_l2tt( l2, &freed );
    }

    translation_table.entry[virt.section++].handler = handler;
    va_pages -= 256;
  }

  if (freed != 0) {
    bool reclaimed = core_claim_lock( &shared.mmu.lock, workspace.core + 1 );
    dll_insert_l2tt_list_at_head( freed, &shared.mmu.free );
    if (!reclaimed) core_release_lock( &shared.mmu.lock );
  }

  if (va_pages > 0) {
    // Memory block ends part way through a section.
    if (translation_table.entry[virt.section].type == 1) {
      l2table = mapped_table( translation_table.entry[virt.section].table );
    }
    else if (translation_table.entry[virt.section].type == 0) {
      memory_fault_handler handler =
                translation_table.entry[virt.section].handler;

      l2table = get_free_table();

      translation_table.entry[virt.section].table = table_entry( l2table );

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

  push_writes_to_cache();
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
static l1tt_entry const dev_section = { .section = { .type2 = 2, .XN = 1 } };
static l2tt_entry const dev_page = { .small_page = 1, .XN = 1 };

void __attribute__(( optimize( "O1" ) )) map_memory( memory_mapping const *mapping )
{
  if (mapping == 0) asm volatile ( "mov r2, lr\n  bkpt 88" );
  if (mapping->pages == 0) {
    asm volatile ( "mov r12, lr" : : "r" (mapping->base_page), "r" (mapping->va) );
    PANIC;
  }

  bool reclaimed = core_claim_lock( &shared.mmu.lock, workspace.core + 1 );

  // Note: Could allow for base_page to be above 4GiB,
  // the extra bits go in the supersection entry, which
  // is the only way to access them in small tables.

  // Could be useful for caches or dynamic areas, at least.

  // To allow for unusual mappings, e.g. six pages in one section,
  // 20 more in the next, this routine may recurse.

  arm32_ptr phys = { .raw = (mapping->base_page << 12) };
  arm32_ptr virt = { .raw = mapping->va };

  bool all_cores = mapping->all_cores;
  bool not_shared = mapping->not_shared;
  int shared_flag = (not_shared && !all_cores) ? 0 : 1;

  // First off, are we mapping pages, large pages, sections,
  // or supersections (or a mixture)?

  // Initially, only pages and sections, can be expanded later.

  if (phys.section_offset == 0
   && virt.section_offset == 0
   && (mapping->pages & 0xff) == 0) {
    // Sections, maybe supersections (TODO), no need to split
    l1tt_entry entry;

    switch (mapping->type) {
    case CK_MemoryRWX: entry.raw = rwx_section.raw | cached_section.raw; break;
    case CK_MemoryRW: entry.raw = rw_section.raw | cached_section.raw; break;
    case CK_MemoryRX: entry.raw = rx_section.raw | cached_section.raw; break;
    case CK_MemoryR: entry.raw = r_section.raw | cached_section.raw; break;
    case CK_Device: entry = dev_section; break;
    default:
      PANIC;
    }

    uint32_t sections = mapping->pages >> 8;

    entry.section.S = shared_flag;

    if (mapping->usr32_access)
      entry.section.unprivileged_access = 1;

    if (mapping->map_specific)
      entry.section.nG = 1;

    entry.section.AF = 1;

    entry.section.base = phys.section;
    uint32_t start = virt.section;
    if (all_cores) {
      for (int i = start; i < start + sections; i++) {
        translation_table.entry[i] = entry;
        global_translation_table.entry[i] = entry;
        entry.section.base++;
      }
    }
    else {
      for (int i = start; i < start + sections; i++) {
        translation_table.entry[i] = entry;
        entry.section.base++;
      }
    }
  }
  else {
    // Pages, for sure
    // Does the mapping include something bigger?
    // Are all the pages in the same section?
    // Not yet implemented!

    // Executive decision: If the first page mapped in a global section
    // is global, the whole section will be global.
    // If not, not.

    // i.e. if the section is not marked as check global or if the first
    // page mapped into a global section is not for all cores, the table
    // will not be shared.

    // TODO: Report an error (or PANIC) if the table is shared, but the
    // page being mapped into it shouldn't be.

    l2tt *table = 0;
    l2tt *global_table = 0;
    l1tt_entry entry = translation_table.entry[virt.section];

#if 0
// Wait until pipes are around, so the UART is enabled
if (translation_table.entry[0x800].type != 0) {
  bool reclaimed = core_claim_lock( &shared.ostask.lock, workspace.core + 1 );
send_number( workspace.core, ':' );
send_number( virt.raw, '>' );
send_number( entry.raw, '\n' );
  if (!reclaimed) core_release_lock( &shared.ostask.lock );
}
#endif

    if (entry.type == 0                         // No table yet
     && entry.handler == check_global_table     // Section check global
     && all_cores) {                            // Mapping global page
      // Has another core already created a table to be shared?
      l1tt_entry global = global_translation_table.entry[virt.section];

      if (global.type == 1) {
        // Yes, share the table.
        translation_table.entry[virt.section] = global;
        entry = global;
      }

      // No? Then we'll make the table in a mo...
    }

    bool new_table = (entry.type == 0);

    if (entry.type == 0) {
      // Don't have a handy shared table (or we don't want to share a table)
      // So, make our own
      memory_fault_handler handler = entry.handler;

      table = get_free_table();

      for (int i = 0; i < 256; i++) {
        table->entry[i].handler = handler;
      }

      entry.table = table_entry( table );

      if (handler == check_global_table) {
        if (all_cores) { // We're going to share the table
          global_translation_table.entry[virt.section] = entry;
        }
        else { // The global mapping needs its own table
          global_table = get_free_table();

          for (int i = 0; i < 256; i++) {
            global_table->entry[i].handler = handler;
          }

          global_translation_table.entry[virt.section].table =
                        table_entry( global_table );
        }
      }
    }
    else if (entry.type == 1) {
      table = mapped_table( entry.table );
      if (all_cores) {
        l1tt_entry global = global_translation_table.entry[virt.section];
        if (global.type != 1) PANIC;
        global_table = mapped_global_table( global.table );
      }
    }

    if (entry.type != 1) PANIC;
    if (table == 0) PANIC;

    l2tt_entry page_entry;

    switch (mapping->type) {
    case CK_MemoryRWX: page_entry.raw = rwx_page.raw | cached_page.raw; break;
    case CK_MemoryRW: page_entry.raw = rw_page.raw | cached_page.raw; break;
    case CK_MemoryRX: page_entry.raw = rx_page.raw | cached_page.raw; break;
    case CK_MemoryR: page_entry.raw = r_page.raw | cached_page.raw; break;
    case CK_Device: page_entry = dev_page; break;
    default:
      PANIC;
    }

    page_entry.S = shared_flag;

    if (mapping->usr32_access)
      page_entry.unprivileged_access = 1;

    if (mapping->map_specific)
      page_entry.nG = 1;

    page_entry.AF = 1;
    page_entry.page_base = phys.page_base;

    if (CK_Device == mapping->type && mapping->pages > 1) PANIC;

    for (int i = 0; i < mapping->pages; i++) {
      table->entry[virt.page] = page_entry;
      if (global_table != 0 && all_cores)
        global_table->entry[virt.page] = page_entry;

      page_entry.page_base++;
      virt.page++;
    }

    push_writes_to_cache();

    if (new_table) {
      translation_table.entry[virt.section] = entry;
      push_writes_to_cache();
    }
  }

  asm ( "DSB"
    "\n  MCR p15, 0, r0, c8, c7, 0 // TLBIALL"  // Overkill - does it get rid of strange_handler?
    "\n  MCR p15, 0, r0, c7, c5, 6 // BPIALL"
    "\n  DSB"
    "\n  ISB" );

  // I misunderstood the meaning of the top bits in a supersection; all
  // entries that are for a supersection must have the same bits 20-31,
  // bits 20-23 are used for the physical address bits above the normal
  // 32-bit address space. G4.4

  // The MMU has been configured to use the cache, so the writes don't
  // have to be flushed to RAM.
  push_writes_to_cache();
  ensure_changes_observable();
  set_way_no_CCSIDR2();
  // ... in theory, but I'm getting stupid data aborts...
  // Apparently fixed by the TLBIALL, above.

  if (!reclaimed) core_release_lock( &shared.mmu.lock );
}

memory_pages walk_global_tree( uint32_t va )
{
  arm32_ptr virt = { .raw = va };
  memory_pages result = { .number_of_pages = 0 };

  l1tt_entry l1 = global_translation_table.entry[virt.section];

  if (l1.type == 0) {
    // No memory mapped at this virtual address
  }
  else if (l1.type == 1) {
    l2tt *table = mapped_table( l1.table );
    l2tt_entry l2 = table->entry[virt.page];

    if (l2.type == 0) {
      // No memory mapped at this virtual address
    }
    else if (l2.type == 1) {
      PANIC; // Large page
    }
    else { // Small page
      // TODO: look for contiguous pages leading up to this?
      // TODO first: look for contiguous pages after this.
      result.number_of_pages = 1;
      result.virtual_base = virt.page_base;
      result.base_page = l2.page_base;
    }
  }
  else {
    // Section
    // TODO: look for contiguous sections leading up to this?
    // TODO first: look for contiguous sections after this.
    result.number_of_pages = 256; // 1 MiB
    result.virtual_base = virt.section << 20;
    result.base_page = l1.section.base << 8;
  }

  return result;
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
        if (check_global_table == l1.handler) {
          return false;
        }
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
        if (check_global_table == l2.handler) {
          PANIC;
          return false;
        }
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

inline uint32_t __attribute__(( always_inline )) get_core_number()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[result], c0, c0, 5" : [result] "=r" (result) );
  return ((result & 0xc0000000) != 0x80000000) ? 0 : (result & 15);
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
    extern uint8_t _romimage_end;
    extern uint8_t _start;
    arm32_ptr high_memory = { .rawp = &_start };
    uint32_t img_size = &_romimage_end - &_start;

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
    extern uint8_t _romimage_end;
    uint32_t free = &_romimage_end - &_start;
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

#ifdef DEBUG__EMERGENCY_UART_ACCESS
  // Emergency UART access. (Assumes it's all set up by the time it's used)
  {
    // Set AF
    // No usr access:
    // l2tt_entry entry = { .raw = 0x3f201413 };
    // With usr32 access
    l2tt_entry entry = { .raw = 0x3f201000 | 0b10000110011 };
    pages[0xff] = entry;

    push_writes_to_cache();
  }
#endif
#if 0
  if (get_core_number() == 0)
  {
    uint32_t volatile *p = (void*) 0x3f200000;
    // GPFSEL bits 14, 15 Alt0
    p[1] = (p[1] & ~(0b111111 << 12)) | (0b100100 << 12);
    p = (void*) 0x3f201000;
    p[0x30/4] &= ~1; // control
    while (p[0x18/4] & 8) { for (int i = 0; i < 1000; i++) asm (""); }
    p[0x24/4] = 1;
    p[0x28/4] = 40;
    p[0x2c/4] = 7 << 4;
    p[0x38/4] = 0;
    p[1] = 0x101;
    p[0] = 'T';
    while (p[0x18/4] & 8) { for (int i = 0; i < 1000; i++) asm (""); }
  }
  else {
    for (int i = 0; i < 0x1000000; i++) asm ("");
  }
#endif

  push_writes_to_cache();
}

void forget_boot_low_memory_mapping()
{
  uint32_t ram_top = (uint32_t) &top_of_boot_RAM;

  clear_memory_region( 0, ram_top >> 12, check_global_table );

  // Clear any TLB using ASID 1
  asm ( "mcr p15, 0, %[one], c8, c7, 2" : : [one] "r" (1) );

  mmu_switch_map( 0 );

  push_writes_to_cache();
}

void mmu_establish_resources()
{
  // This is probably an extreme number of tables, TODO see how 
  // many we actually need and adjust the algorithms accordingly.
  // If 1 MiB more or less becomes a problem!

  extern l2tt VMSAv6_Level2_Tables[4096];
  l2tt * volatile *free = &shared.mmu.free;

  if (0 == change_word_if_equal( (uint32_t*) &shared.mmu.free, 0, 1 )) {
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
    l2tt *pool = 0;
    // for (int i = 0; i < number_of( VMSAv6_Level2_Tables ); i++) {
    for (int i = 0; i < 64; i++) {
      l2tt *t = &VMSAv6_Level2_Tables[i];
      dll_new_l2tt( t );
      dll_attach_l2tt( t, &pool );
    }
    *free = pool;
  }
  else {
    while (*free == (l2tt *) 1) {}
    // Ensure the pool can be seen by every core before trying to use it.
    asm ( "" : : "r" (shared.mmu.free->entry[0].raw) );
  }
}

// Real hardware reports errors type 0x007, at fa 0x80000000+
// Real hardware reports errors type 0x807, at fa 0x80000000+
// Seems to have been from a lack of a synchonisation primitive, but IDK
// which one!
static bool strange_handler( uint32_t fa, uint32_t ft )
{
#ifdef DEBUG__REPORT_STRANGE_HANDLER
  bool reclaimed = core_claim_lock( &shared.ostask.lock, workspace.core + 1 );
  send_number( (uint32_t) workspace.ostask.running, ' ' );
  send_number( fa, ' ' );
  send_number( ft, '\n' );

  uint32_t *p = &ft;
  for (int i = 0; i < 16; i++) 
    send_number( p[i], '\n' );

  arm32_ptr va = { .raw = fa };

  l1tt_entry l1 = translation_table.entry[va.section];
  send_number( l1.raw, ':' );
  l2tt_entry l2 = {};
  if (l1.type == 1) {
    l2tt *table = mapped_table( l1.table );
    l2 = table->entry[va.page];
  }
  send_number( l2.raw, '\n' );

  core_release_lock( &shared.ostask.lock );
  //for (;;) {}
#endif

  return true;
}

static __attribute__(( noinline )) memory_fault_handler find_handler( uint32_t fa )
{
  arm32_ptr va = { .raw = fa };
  l1tt_entry l1 = translation_table.entry[va.section];
  switch (l1.type) {
  case 0:
    return l1.handler;
  case 1:
    {
      l2tt *table = mapped_table( l1.table );
      l2tt_entry l2 = table->entry[va.page];
      if (l2.type != 0) return strange_handler; // PANIC;
      return l2.handler;
    }
  default: return strange_handler; // PANIC;
  }
  return 0;
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

__attribute__(( weak, noinline, noreturn ))
void signal_data_abort( svc_registers *regs, uint32_t fa, uint32_t ft )
{
  PANIC;
}

__attribute__(( weak, noinline, noreturn ))
void instruction_abort( svc_registers *regs, enum AbtType type )
{
  PANIC;
}

void __attribute__(( naked, noreturn )) data_abort_handler()
{
  asm volatile (
        "  sub lr, lr, #8"
      "\n  srsdb sp!, #0x17 // Store return address and SPSR"
      "\n  push { "C_CLOBBERED" }"
      );

  if (!handle_data_abort()) {
    register svc_registers *regs asm( "r0" );
    asm ( "pop { "C_CLOBBERED" }"
      "\n  push { r0-r12 }"
      "\n  mov r0, sp"
      : "=r" (regs)
      :
      : "r1", "r2", "r3", "ip" );
    signal_data_abort( regs, fault_address(), data_fault_type() );
    PANIC;
  }

  asm ( "DSB"
    "\n  MCR p15, 0, r0, c8, c7, 0 // TLBIALL"  // Overkill - does it get rid of strange_handler? Yes!
    // But check_global still fails...
    "\n  MCR p15, 0, r0, c7, c5, 6 // BPIALL"
    "\n  DSB"
    "\n  ISB" );

  asm volatile ( "pop { "C_CLOBBERED" }"
    "\n  rfeia sp! // Restore execution and SPSR" );

  __builtin_unreachable();
}

static bool __attribute__(( noinline )) handle_prefetch_abort( uint32_t fa, uint32_t ft )
{
  // This subsystem can cope with translation faults (treating them the
  // same as data faults), anything else (permissions, access flags, etc.)
  // need to be dealt with elsewhere.

  if ((ft & ~0x8f0) == 7
   || (ft & ~0x8f0) == 5) {
    memory_fault_handler handler = find_handler( fa );

    if (handler == 0) return false;

    return handler( fa, ft );
  }

  return false;
}

enum AbtType generic_abort_type( uint32_t ft )
{
  switch (ft & 0x40f) {
  case 1: return ABT_ALIGN;
  case 5:
  case 7: return ABT_TRANSLATION;
  case 13:
  case 15: return ABT_PERMISSION;
  default: return ABT_SPECIAL;
  };
}

void __attribute__(( naked )) prefetch_handler()
{
  register uint32_t fa asm( "r0" );
  register svc_registers *regs;
  asm volatile (
    "\n.ifne .-prefetch_handler"
    "\n  .error \"prefetch_handler check generated code\""
    "\n.endif"
    "\n  sub lr, lr, #4"        // Is this safe? IFAR instead?
    "\n  srsdb sp!, #0x17 // Store fail address and SPSR (Abt mode)"
    "\n  push {r0-r12}"
    "\n  mov r0, lr"
    "\n  mov %[regs], sp"
    : "=r" (fa)
    , [regs] "=r" (regs)
  );

  register uint32_t ft = instruction_fault_type();

  if (!handle_prefetch_abort( fa, ft )) {
    instruction_abort( regs, generic_abort_type( ft ) );
  }

  // IDK if this is approprate in prefetch, it's c-n-p from data abort
  asm ( "DSB"
    "\n  MCR p15, 0, r0, c8, c7, 0 // TLBIALL"  // Overkill - does it get rid of strange_handler? Yes!
    // But check_global still fails...
    "\n  MCR p15, 0, r0, c7, c5, 6 // BPIALL"
    "\n  DSB"
    "\n  ISB" );

  asm volatile ( "pop {r0-r12}"
    "\n  rfeia sp! // Restore execution and SPSR" );

  __builtin_unreachable();
}

void mmu_switch_map( uint32_t new_map )
{
  // CONTEXTIDR
  asm ( "mcr p15, 0, %[map], c13, c0, 1" : : [map] "r" (new_map) );
}

void forget_current_map()
{
  uint32_t map;
  // CONTEXTIDR
  asm ( "mrc p15, 0, %[map], c13, c0, 1" : [map] "=r" (map) );
  asm ( "mcr p15, 0, %[map], c8, c7, 2" : : [map] "r" (map) );
}

