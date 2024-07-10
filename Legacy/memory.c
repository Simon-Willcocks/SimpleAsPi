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
#include "doubly_linked_list.h"
#include "ostask_extras.h"
#include "raw_memory_manager.h"

#include "ostaskops.h"

DEFINE_ERROR( UnknownDA, 0x105, "Unknown dynamic area" );

// This should probably mostly be farmed out to a memory manager module...

// To start with, ignore most things and simply allocate the maximum size

struct dynamic_area {
  dynamic_area *next;
  dynamic_area *prev;
  uint32_t number;
  uint32_t va_start;
  uint32_t max_size;
  uint32_t current_size;

  // TODO: Call handlers, as required.
  uint32_t handler;
  uint32_t workspace;
  char name[];
};

static int strlen( char const *p )
{
  char const *e = p;
  while (*e >= ' ') e++;
  return e - p;
}

DLL_TYPE( dynamic_area )

static dynamic_area *find_da( uint32_t num )
{
  dynamic_area *da = shared.legacy.dynamic_areas;
  if (da == 0) return 0;

  while (da->number != num &&
         da->next != shared.legacy.dynamic_areas)
    da = da->next;

  if (da->number != num) return 0;

  return da;
}

void do_OS_ReadDynamicArea( svc_registers *regs )
{
  bool return_max = regs->r[0] >= 128;
  uint32_t max = 0;
  uint32_t number = regs->r[0];
  if (number < 256 && number >= 128) number -= 128;

  switch (number) {
  case -1: // Application space
    {
      extern uint8_t app_memory_limit;
      extern uint32_t app_memory_top( uint32_t new );
      uint32_t size = (&app_memory_limit - (uint8_t*)0) - 0x8000;
      regs->r[0] = 0x8000;
      regs->r[1] = app_memory_top( 0 ); // Read
      regs->r[2] = size;
    }
    break;
  case 0: // System Heap
    {
      extern uint8_t system_heap_base;
      extern uint8_t system_heap_top;
      regs->r[0] = (uint32_t) &system_heap_base;
      regs->r[1] = &system_heap_top - &system_heap_base;
      max = regs->r[1];
    }
    break;
  case 1: // RMA
    {
      extern uint8_t shared_heap_base;
      extern uint8_t shared_heap_top;

      regs->r[0] = (uint32_t) &shared_heap_base;
      regs->r[1] = &shared_heap_top - &shared_heap_base;
      max = regs->r[1];
    }
    break;
  default:
    {
      dynamic_area *da = find_da( regs->r[0] );

      if (da != 0) {
        if (regs->r[0] > 127)
          regs->r[2] = da->max_size;
        regs->r[0] = da->va_start;
        regs->r[1] = da->current_size;
      }
      else {
        Error_UnknownDA( regs );
        return;
      }
    }
    break;
  }

  if (return_max) {
    regs->r[2] = max;
  }
}

extern uint32_t dynamic_areas_base;
extern uint32_t dynamic_areas_top;

static uint32_t const da_base = (uint32_t) &dynamic_areas_base;
static uint32_t const da_top = (uint32_t) &dynamic_areas_top;

void do_OS_DynamicArea( svc_registers *regs )
{
  enum {
    Create, Delete, Info, Enumerate,
    Renumber, FreeSpace, Internal6, Internal7,
    SetClamps, LockSparse, ReleaseSparse, LockArea,
    UnlockArea, ResizeArea, DescribeArea, ClaimBlock,
    ReleaseBlock, ResizeBlock, ReadBlockSize, ChangeDomain,
    LocateByAddress, PMPPhysOp, PMPLogicalOp, PMPResizeOp,
    PMPInfo, ExaminePMPPages, AdjustAppSpaceLimit, TotalFreePages,
    Internal28
  };
  uint32_t action = regs->r[0];
  switch (action) {
  case Create:
    {
      char const *name = (void*) regs->r[8];
      int len = (strlen( name ) + 4 & ~3);
      dynamic_area *new_da = system_heap_allocate( sizeof( *new_da ) + len );
      dll_new_dynamic_area( new_da );
      new_da->number = regs->r[1];
      if (new_da->number == -1) {
        if (shared.legacy.last_allocated_da == 0) {
          shared.legacy.last_allocated_da = -1;
        }
        new_da->number = --shared.legacy.last_allocated_da;
        regs->r[1] = new_da->number;
      }
      new_da->current_size = regs->r[2];
      if (regs->r[3] != -1) PANIC;
      if (shared.legacy.last_da_top == 0) shared.legacy.last_da_top = da_top;

      if (regs->r[5] == -1) regs->r[5] = 16 << 20;

      if (shared.legacy.last_da_top - regs->r[5] < da_base) PANIC; // FIXME

      shared.legacy.last_da_top -= regs->r[5];
      new_da->va_start = shared.legacy.last_da_top;
      regs->r[3] = new_da->va_start;

      new_da->handler = regs->r[6];
      new_da->workspace = regs->r[7];
      char *d = new_da->name;
      while (*name >= ' ') { *d++ = *name++; }

      uint32_t pages = (0xfff + regs->r[5]) >> 12;
      if (pages > 0x400) {
        pages = 0x400;
        Task_LogString( "Limiting DA size\n", 0 );
      } // 4 MiB FIXME

      uint32_t physical = claim_contiguous_memory( pages );
      if (physical == 0) PANIC;

      memory_mapping map = {
        .base_page = physical,
        .pages = pages,
        .va = new_da->va_start,
        .type = CK_MemoryRW,
        .map_specific = 0,
        .all_cores = 1,
        .usr32_access = 1 };

      map_memory( &map );

      dll_attach_dynamic_area( new_da, &shared.legacy.dynamic_areas );
    }
    break;
  case Info:
    {
      if (regs->r[1] == 6) {
        // Free pool, faking it
        regs->r[2] = 16 << 20; // 16MiB
        regs->r[3] = 0xbadf00d;
        regs->r[4] = 0; // 
        regs->r[5] = 16 << 20; // 16MiB
        regs->r[6] = 0;
        regs->r[7] = 0;
        regs->r[8] = (uint32_t) "Free";
        return;
      }

      dynamic_area *da = find_da( regs->r[1] );

      if (da != 0) {
        regs->r[2] = da->current_size;
        regs->r[3] = da->va_start;
        regs->r[4] = 0; // TODO?
        regs->r[5] = da->max_size;
        regs->r[6] = da->handler;
        regs->r[7] = da->workspace;
        regs->r[8] = da->name;
      }
      else {
        Error_UnknownDA( regs );
        return;
      }
    }
    break;
  case Enumerate:
    {
      dynamic_area *da;
      if (regs->r[1] == -1) {
        da = shared.legacy.dynamic_areas;
      }
      else {
        da = find_da( regs->r[1] );
      }
      if (da->next == shared.legacy.dynamic_areas) {
        regs->r[1] = -1;
      }
      else {
        regs->r[1] = da->next->number;
      }
    }
    break;
  case 27: // Tested by WindowManager
    {
    regs->spsr |= VF;
    // FIXME
    }
    break;
  default:
    for (;;) { asm ( "wfi" ); }
  }
}

void do_OS_ChangeDynamicArea( svc_registers *regs )
{
  // TODO whole pages
  dynamic_area *da = find_da( regs->r[0] );
  if (da == 0) PANIC;
  da->current_size += regs->r[1];
  if (0 > (int32_t) regs->r[1]) {
    regs->r[1] = -regs->r[1];
  }
}

void do_OS_Memory( svc_registers *regs )
{
  enum {
    General, res1, res2, res3,
    res4, res5, ReadTableSize, ReadTable,
    ReadAvailable, FindController, PoolLock, res11,
    RecommendPage, MapInIOPermanent, MapInIOTemporary, MapOutTemporary,
    MemoryAreas, MemoryAccessPrivileges, FindAccessPrivilege, PrepareForDMA,
    CompatibilitySettings, MapInIOPermanent64, MapInIOTemporary64, ReserveRAM,
    CheckMemoyAccess, FindControllerROL, General64Bit, VAtoPA };

  uint32_t flags = regs->r[0] >> 8;
  uint8_t code = regs->r[0] & 0xff;
  switch (code) {
  case General:
    {
      // 0x2200 converts the logical address to physical
      // But why is it called in the kernel?? Essentially ignoring for now
      // Kernel/s/vdu/vdudriver Line 201. Getting physical address of 256 byte
      // cursor blocks (six times over).
      if (flags != 0x22) asm ( "bkpt 1" );

      struct __attribute__(( packed )) {
        uint32_t physical_page;
        uint32_t logical_address;
        uint32_t physical_address;
      } *page_blocks = (void*) regs->r[1];

      Task_LogString( "Virtual to physical addresses (", 0 );
      Task_LogHex( regs->lr );
      Task_LogString( ")\n", 2 );
      for (int i = 0; i < regs->r[2]; i++) {
        Task_LogHex( page_blocks[i].physical_page );
        Task_LogString( " ", 1 );
        Task_LogHex( page_blocks[i].logical_address );
        Task_LogString( " ", 1 );
        Task_LogHex( page_blocks[i].physical_address );
        Task_LogNewLine();
        page_blocks[i].physical_address = 0x75750000;
            //Task_PhysicalFromVirtual( page_blocks[i].logical_address, 4 );
      }
    }
    break;
  case PoolLock:
    break;
  case MapInIOPermanent:
    {
      // One shot! FIXME
      // Works for the display on initial mode "change"
      uint32_t phys = regs->r[1];
      uint32_t bytes = regs->r[2];

      uint32_t base = 0xc0000000;

      bool bufferable = 0 != (flags & 1);
      bool cacheable = 0 != (flags & 2);
      int policy = (flags >> 2) & 7;
      bool double_map = 0 != (flags & 16);
      bool set_access_privileges = 0 != (flags & 17);
      int access_privileges = (flags >> 16) & 15;

  Task_LogString( "OS_Memory MapInIOPermanent, flags ", 0 );
  Task_LogHex( flags );
  Task_LogString( ", base ", 0 );
  Task_LogHex( phys );
  Task_LogString( ", size ", 0 );
  Task_LogHex( bytes );
  Task_LogString( ", at ", 0 );
  Task_LogHex( base );
  if ((flags & (1 << 8)) != 0) Task_LogString( ", doubled", 0 );
  Task_LogNewLine();
  Task_LogString( "IGNORED! Done by BCM2835Display.", 0 );
  Task_LogNewLine();
/*
      memory_mapping mapping = {
        .base_page = (phys) >> 12,
        .pages = bytes >> 12,
        .va = base,
        .type = CK_MemoryRW,
        .map_specific = 0,
        .all_cores = 1,
        .usr32_access = 1 };
      map_memory( &mapping );

      if ((flags & (1 << 8)) != 0) {
        memory_mapping mapping = {
          .base_page = phys >> 12,
          .pages = bytes >> 12,
          .va = base + bytes,
          .type = CK_MemoryRW,
          .map_specific = 0,
          .all_cores = 1,
          .usr32_access = 1 };
        map_memory( &mapping );
      }
*/
uint32_t *screen = (void*) base;
uint32_t colour = 0x12845678;
for (int y = 0; y < 1080; y++) {
  for (int x = 0; x < 1920; x++) {
    screen[x + 1920 * y] = colour++;
  }
}
      regs->r[3] = base;
    }
    break;
  default: PANIC;
  };
}

void do_OS_ValidateAddress( svc_registers *regs )
{
  // FIXME: Don't assume always good!
  // Application memory, dynamic areas, system and shared heap, etc.
  // I think it's fair to complain if the memory region stradles two
  // logical regions.
  regs->spsr &= ~CF;
}

