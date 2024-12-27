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
#include "kernel_swis.h"

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
  uint32_t actual_pages; // Pages allocated and mapped in
  uint32_t pages;       // Pages a program will be told about

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

static uint32_t new_da_number()
{
  if (shared.legacy.last_allocated_da == 0) {
    shared.legacy.last_allocated_da = 0x41450000;
  }
  return --shared.legacy.last_allocated_da;
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
        max = da->max_size;
        regs->r[0] = da->va_start;
        regs->r[1] = da->pages << 12;
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

static error_block *resize_da( dynamic_area *da, int32_t resize_by_pages )
{
  if (resize_by_pages == 0) { // Doing nothing
    return 0;
  }

  if (resize_by_pages < 0 && (-resize_by_pages > da->pages)) {
    resize_by_pages = -da->pages;      // Attempting to reduce the size as much as possible
  }

  int32_t resize_by = resize_by_pages << 12;

  if (((da->pages + resize_by_pages) << 12) > da->max_size) {
    static error_block error = { 999, "DA maximum size exceeded" };
    // asm ( "bkpt 21" );
    return &error;
  }

  if (da->handler != 0 && resize_by < 0) {
    // Pre-shrink
    register uint32_t code asm ( "r0" ) = 2;
    register uint32_t shrink_by asm ( "r3" ) = -resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workspace == -1 ? da->va_start : da->workspace;
    register error_block const *error asm( "r0" );
    register int32_t permitted asm ( "r3" );
    asm ( "blx %[preshrink]"
      "\n  movvc r0, #0"
        : "=r" (error)
        , "=r" (permitted)
        : "r" (code)
        , "r" (shrink_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [preshrink] "r" (da->handler) 
        : "lr" );
    if (error != 0) { // pre-shrink code
      return error;
    }
    permitted = -permitted; // FIXME: Non-page multiples
    resize_by_pages = permitted >> 12;
    resize_by = resize_by_pages << 12;
  } 
  else if (da->handler != 0 && resize_by >= 0) {
    // Pre-grow
    register uint32_t code asm ( "r0" ) = 0;
    register uint32_t page_block asm ( "r1" ) = 0xbadf00d;
    register uint32_t pages asm ( "r2" ) = resize_by_pages;
    register uint32_t grow_by asm ( "r3" ) = resize_by;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workspace == -1 ? da->va_start : da->workspace;
    register error_block const *error asm( "r0" );
    asm ( "blx %[pregrow]"
      "\n  movvc r0, #0"
        : "=r" (error)
        : "r" (code)
        , "r" (page_block)
        , "r" (pages)
        , "r" (grow_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [pregrow] "r" (da->handler)
        : "lr" );
    if (error != 0) {
      return error;
    }
  } 

  // TODO Actually release memory as it's not used.
  // mmu_release_global_pages( va, pages ); (Wakes a task to do the job,
  // cleaning out the memory map on each core then returning the memory to
  // the free pool?)

  da->pages = da->pages + resize_by_pages; // Always increased (or decreased) to sufficient pages
  // A small increase of less than a page may not change the
  // number of pages.
  if (da->pages > da->actual_pages) {
    uint32_t new_pages = da->pages - da->actual_pages;
    uint32_t physical = claim_contiguous_memory( new_pages );
    if (physical == 0) PANIC;

    Task_LogString( "Expanding DA ", 13 );
    Task_LogSmallNumber( da->number );
    Task_LogString( " by ", 4 );
    Task_LogSmallNumber( resize_by_pages );
    Task_LogString( " pages, to 0x", 12 );
    Task_LogHex( da->pages << 12 );
    Task_LogNewLine();
    uint32_t new_va = da->va_start + (da->actual_pages << 12);
    memory_mapping map = {
        .base_page = physical,
        .pages = new_pages,
        .va = new_va,
        .type = CK_MemoryRW,
        .map_specific = 0,
        .all_cores = 1,
        .usr32_access = 1 };

    map_memory( &map );

    da->actual_pages = da->pages;
  }

  if (da->handler != 0 && resize_by >= 0) {
    // Post-grow
    register uint32_t code asm ( "r0" ) = 1;
    register uint32_t page_block asm ( "r1" ) = 0xbadf00d;
    register uint32_t pages asm ( "r2" ) = resize_by_pages;
    register uint32_t grown_by asm ( "r3" ) = resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workspace == -1 ? da->va_start : da->workspace;
    register error_block const *error asm( "r0" );
    asm ( "blx %[postgrow]"
      "\n  movvc r0, #0"
        : "=r" (error)
        : "r" (code)
        , "r" (page_block)
        , "r" (pages)
        , "r" (grown_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [postgrow] "r" (da->handler) 
        : "lr" );
    if (error != 0) { // Changed
      return error;
    }
  }

  if (da->handler != 0 && resize_by < 0) {
    // Post-shrink
    register uint32_t code asm ( "r0" ) = 3;
    register uint32_t grown_by asm ( "r3" ) = resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workspace == -1 ? da->va_start : da->workspace;
    register error_block const *error asm( "r0" );
    asm ( "blx %[postgrow]"
      "\n  movvc r0, #0"
        : "=r" (error)
        : "r" (code)
        , "r" (grown_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [postgrow] "r" (da->handler) 
        : "lr" );
    if (error != 0) { // Changed
      return error;
    }
  }

  {
    // Service_MemoryMoved
    register uint32_t code asm( "r1" ) = 0x4e;
    asm ( "svc %[swi]"
        :
        : [swi] "i" (OS_ServiceCall | Xbit)
        , "r" (code)
        : "lr", "cc", "memory" );
  }

  return 0;
}

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
      int len = (strlen( name ) + 4) & ~3;
      dynamic_area *new_da = system_heap_allocate( sizeof( *new_da ) + len );
      dll_new_dynamic_area( new_da );

      if (regs->r[5] == -1) regs->r[5] = 16 << 20; // Max size

      new_da->pages = 0;
      new_da->max_size = regs->r[5];
      new_da->actual_pages = 0;
      new_da->number = regs->r[1];

      // TODO: area flags

      if (new_da->number == -1) {
        new_da->number = new_da_number();
        regs->r[1] = new_da->number;
      }

      if (regs->r[3] != -1) PANIC;
      if (shared.legacy.last_da_top == 0) shared.legacy.last_da_top = da_top;

      if (shared.legacy.last_da_top - regs->r[5] < da_base) PANIC; // FIXME

      shared.legacy.last_da_top -= regs->r[5];
      new_da->va_start = shared.legacy.last_da_top;
      regs->r[3] = new_da->va_start;

      new_da->handler = regs->r[6];
      new_da->workspace = regs->r[7];
      char *d = new_da->name;
      while (*name >= ' ') { *d++ = *name++; }

      uint32_t pages = (0xfff + regs->r[2]) >> 12;

      resize_da( new_da, pages );

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
        regs->r[2] = da->pages << 12;
        regs->r[3] = da->va_start;
        regs->r[4] = 0; // TODO?
        regs->r[5] = da->max_size;
        regs->r[6] = da->handler;
        regs->r[7] = da->workspace;
        regs->r[8] = (uint32_t) da->name;
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
  dynamic_area *da = find_da( regs->r[0] );

  if (da == 0) {
    Error_UnknownDA( regs );
    return;
  }

  int32_t resize_by = (int32_t) regs->r[1];
  int32_t resize_by_pages = resize_by >> 12;

  if (0 != (resize_by & 0xfff)) {
    resize_by_pages ++;
  }

  error_block *error = resize_da( da, resize_by_pages );
  if (error) {
    regs->r[0] = (uint32_t) error;
    regs->spsr |= VF;
    PANIC;
  }
  else {
    regs->r[1] = resize_by_pages;
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
      bool bufferable = 0 != (flags & 1);
      bool cacheable = 0 != (flags & 2);
      int policy = (flags >> 2) & 7;
      bool double_map = 0 != (flags & 16);
      bool set_access_privileges = 0 != (flags & 17);
      int access_privileges = (flags >> 16) & 15;

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
uint32_t *screen = (void*) base;
uint32_t colour = 0x12845678;
for (int y = 0; y < 1080; y++) {
  for (int x = 0; x < 1920; x++) {
    screen[x + 1920 * y] = colour++;
  }
}
*/
      regs->r[3] = base;
    }
    break;
  case ReadAvailable:
    {
      enum { DRAM = 1, VRAM, ROM, IO, Soft_ROM };
      switch (regs->r[0] >> 8) {
      case Soft_ROM:
        regs->r[1] = 6 << 8; // FIXME!
        regs->r[2] = 4096;
        break;
      default:
        PANIC;
      }
    }
    break;
  case CheckMemoyAccess:
    {
      // FIXME
      // All memory is readable and writable! Honest!
      // TODO check the slot.
      regs->r[1] = 0xf;
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

void do_OS_AMBControl( svc_registers *regs )
{
  if (regs->r[0] == 1) return; // Deallocate slot FIXME
  PANIC;
}

