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

// Memory management:
// 32-bit page numbers can deal with up to 16,384 GiB of RAM

typedef enum {  CK_MemoryRWX,
                CK_MemoryRW,
                CK_MemoryRX,
                CK_MemoryR,
                CK_Device } CK_Memory;

// Handlers either use one of the map_... functions to remove
// the fault or return false if there's nothing it can do.
// (One possible action to remove the fault is to replace the
// running task with one that will not fault. This code doesn't
// have to know about that.)
typedef bool (*memory_fault_handler)( uint32_t va, uint32_t fault );

typedef struct __attribute__(( packed )) {
  uint32_t base_page;
  uint32_t pages;
  union {
    uint32_t va;
    void    *vap;
  };
  uint32_t type:8;
  bool     global:1;
  bool     shared:1;
  bool     application_memory:1;
  uint32_t res:20;
} memory_mapping;

#define mmu_section_size (1 << 20)
// 1MiB for short descriptors,
// 2MiB for long

#define mmu_page_size (1 << 12)

// For shared_workspace and core_workspace:
#include "system_workspaces.h"
#define P( s ) (((s) + mmu_page_size - 1) >> 12)

#define L1TT 0x4000
#define L2TT 0x0400

// Must be 16KiB aligned for all cores (the Level 1 TT is at the
// start of it), so CORE_WORKSPACE must be a multiple of 16KiB.
// (We can always free unused workspace, or use it for something else.)
#define CORE_WORKSPACE (((P(sizeof( core_workspace )) + P(L1TT) + P(L2TT) + 3) & ~3) << 12)

// Use the first workspace_needed_for_empty_translation_tables bytes
// of workspace to generate translation tables for a 32-bit memory
// map that has this OS image (as a multiple of mmu_section_size) at
// its proper location (given by the virtual address of _start,
// 0xfc000000 at the time of writing), and sufficient lower level 
// translation tables to be able to map mmu_page_size pages in at 
// least the top MiB.
// RAM from 0 to top_of_boot_RAM will also be mapped rwx at VA 0.
void create_default_translation_tables( uint32_t workspace );

// Call this (once) when the core is running in high memory.
void forget_boot_low_memory_mapping();

// Not to be called before create_default_translation_tables:
void clear_memory_region(
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler );

void map_memory( memory_mapping const *mapping );

// Not quite sure if these should be in processor.h, or even
// processor.c
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

