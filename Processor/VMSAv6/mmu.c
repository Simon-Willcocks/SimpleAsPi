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
  uint32_t section_base:12;
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
    uint32_t section_offset:20;
  };
} arm32_ptr;

void clear_memory_region(
                uint32_t *translation_table,
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler )
{
  arm32_ptr virt = { .raw = va_base };
  arm32_ptr top = { .raw = va_base + (va_pages << 12) };

  if (0 != top.section_offset
   || 0 != (va_pages & 0xff) // Only sections (L1TT)
   || 0 != virt.section_offset) for (;;) asm ( "wfi" );

  for (int i = virt.section; i < top.section; i++) {
    translation_table[i] = (uint32_t) handler;
  }
}

#define GLOBAL 256

static inline
void map_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                uint32_t memory_type_or_global )
{
  // Note: Could allow for base_page to be above 4GiB,
  // the extra bits go in the supersection entry, which
  // is the only way to access them in small tables.

  arm32_ptr phys = { .raw = (base_page << 12) };
  arm32_ptr virt = { .raw = va };

  // Only sections and supersections at the moment
  bool global = (memory_type_or_global & GLOBAL) != 0;
  CK_Memory type = (memory_type_or_global & ~GLOBAL);

  switch (type) {
  case CK_Device:
    {
      if (0 != phys.section_offset) for (;;) asm ( "wfi" );
      uint32_t device_section = 0x00030c12;
      if (!global) device_section |= (1 << 17); // nG
      translation_table[virt.section] = (phys.section << 20) | device_section;
      goto done;
    }
  case CK_MemoryRWX:
    break;
  default:
    for (;;) asm( "wfi" );
  }

  if (0 != phys.section_offset  // MiB boundaries (to start with)
   || 0 != (phys.section & 0xf) // In fact, only supersections
   || 0 != (pages & 0xfff)
   || 0 != virt.section_offset  // Ditto for VA
   || 0 != (virt.section & 0xf)) for (;;) asm ( "wfi" );

//  uint32_t supersection_flags = 0x00075c06;
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

// Note: ROM segments: 75c06 =>
// Least to most significant bits:
//      Type 2 (segment)
//      B = 1   Since TEX[2] == 1:
//      C = 0     Write back, Read allocate, Write Allocate (inner)
//      XN = 0 (executable)
//      Domain = 0 (basically obsolete)
//      P = 0
//      AF = 0 (Don't tell me when first accessed)
//      Unprivileged access = 1 (but only until boot)
//      TEX = 5 => Write back, Read allocate, Write Allocate (outer)
//      Read-write
//      Shared
//      ASID-associated (not Global)
//      Supersection
//      Secure

// I had misunderstood the meaning of the top bits; all entries that
// are for a supersection must have the same bits 20-31, bits 20-23
// are used for the physical address bits above the normal 32-bit
// address space. G4.4

  for (int i = 0; i < pages >> 8; i++) {
    translation_table[virt.section + i] =
                ((phys.section << 20) & 0xff000000) | supersection.raw;
  }

done:
  // The MMU has been configured to use the cache...
  push_writes_to_cache();
}

void map_app_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                CK_Memory memory_type )
{
  map_memory( translation_table, base_page, pages, va, memory_type );
}

void map_global_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                CK_Memory memory_type )
{
  map_memory( translation_table, base_page, pages, va, memory_type | GLOBAL );
}

