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

// Defined at link time:
extern l1tt_entry translation_table[4096];
extern l1tt_entry global_translation_table[4096];
extern l2tt_entry local_kernel_page_table[256];
extern l2tt_entry global_kernel_page_tables[4][256];
extern uint8_t top_of_boot_RAM;

void clear_memory_region(
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler )
{
  arm32_ptr virt = { .raw = va_base };
  arm32_ptr top = { .raw = va_base + (va_pages << 12) };

  if (0 != top.section_offset
   || 0 != (va_pages & 0xff) // Only sections (L1TT)
   || 0 != virt.section_offset) for (;;) asm ( "wfi" );

  for (int i = virt.section; i < top.section; i++) {
    translation_table[i].handler = handler;
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
  if (mapping->pages == 0) return;

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
      for (;;) asm( "wfi" );
    }

    uint32_t sections = mapping->pages >> 8;

    l1tt_entry entry = { .raw = l1.raw | cached_section.raw };
    entry.section.base = phys.section;
    for (int i = 0; i < sections; i++) {
      translation_table[virt.section + i] = entry;
      entry.section.base++;
    }
  }
  else {
    // Pages, for sure
    // Does the mapping include something bigger?
    // Are all the pages in the same section?
    // Not yet implemented!
    if (virt.section != 0xfff) asm ( "udf #1\nwfi" );

    l2tt_entry l2;

    switch (mapping->type) {
    case CK_MemoryRWX: l2 = rwx_page; break;
    case CK_MemoryRW: l2 = rw_page; break;
    case CK_MemoryRX: l2 = rx_page; break;
    case CK_MemoryR: l2 = r_page; break;
    case CK_Device: l2 = dev_page; break;
    default:
      for (;;) asm( "wfi" );
    }

    l2.AF = 1;
    l2.page_base = phys.page_base;

    local_kernel_page_table[virt.page] = l2;
  }

  // I misunderstood the meaning of the top bits in a supersection; all
  // entries that are for a supersection must have the same bits 20-31,
  // bits 20-23 are used for the physical address bits above the normal
  // 32-bit address space. G4.4

  // The MMU has been configured to use the cache, so the writes don't
  // have to be flushed to RAM.
  push_writes_to_cache();
}

void create_default_translation_tables( uint32_t memory )
{
  l1tt_entry *tt = (void*) memory;
  l2tt_entry *pages = (void *) (&tt[4096]);

  for (int i = 0; i < 4096; i++) {
    tt[i].handler = 0;
  }

  for (int i = 0; i < 256; i++) {
    pages[i].handler = 0;
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

    if (0 != high_memory.section_offset) asm ( "wfi" );

    // static l1tt_entry const rx_cached_section = { .section = { .type2 = 2, .XN = 0, .read_only = 1, .TEX = 5, .C = 0, .B = 1, .AF = 1 } };
    // l1tt_entry entry = { .raw = rx_cached_section.raw | high_memory };
    // The above doesn't work here because the code generated accesses high memory

    l1tt_entry entry = { .raw = 0x0000d406 };
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
    arm32_ptr p = { .rawp = &translation_table[0] };
    l2tt_entry entry = { .raw = 0x157 | memory };
    pages[p.page+0] = entry; entry.page_base++;
    pages[p.page+1] = entry; entry.page_base++;
    pages[p.page+2] = entry; entry.page_base++;
    pages[p.page+3] = entry;
  }
  {
    arm32_ptr p = { .rawp = &global_translation_table[0] };
    // Shared, RW
    l2tt_entry entry = { .raw = 0x557 | core0_workspace };
    pages[p.page+0] = entry; entry.page_base++;
    pages[p.page+1] = entry; entry.page_base++;
    pages[p.page+2] = entry; entry.page_base++;
    pages[p.page+3] = entry;
  }
  {
    arm32_ptr p = { .rawp = &local_kernel_page_table };
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
    translation_table[i].handler = 0;
  }

  push_writes_to_cache();

  // Clear any TLB using ASID 1
  asm ( "mcr p15, 0, %[one], c8, c7, 2" : : [one] "r" (1) );

  push_writes_to_cache();
}

