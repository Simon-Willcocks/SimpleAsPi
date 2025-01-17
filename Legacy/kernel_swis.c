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
#include "ostask.h"
#include "qa7.h"
#include "bcm_gpio.h"
#include "bcm_uart.h"
#include "heap.h"
#include "raw_memory_manager.h"
#include "legacy.h"
#include "kernel_swis.h"
#include "ZeroPage.h"
#include "memory.h"

extern uint8_t system_heap_base;
extern uint8_t system_heap_top;
// Shared heap of memory that's user rwx
extern uint8_t shared_heap_base;
extern uint8_t shared_heap_top;

extern uint32_t JTABLE[128];

void heap_initialise( void *start, uint32_t size )
{
  register uint32_t command asm ( "r0" ) = 0;
  register void *heap asm ( "r1" ) = start;
  register uint32_t r2 asm ( "r2" ) = size;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OS_Heap), "r" (command), "r" (heap), "r" (r2)
    : "lr", "cc" );
}

void *heap_allocate( void *start, uint32_t size )
{
  register uint32_t command asm ( "r0" ) = 2;
  register void *heap asm ( "r1" ) = start;
  register void *mem asm ( "r2" );      // OUT
  register uint32_t bytes asm ( "r3" ) = size;
  asm ( "svc %[swi]"
    : "=r" (mem)
    : [swi] "i" (OS_Heap), "r" (command), "r" (heap), "r" (bytes)
    : "lr", "cc" );
  return mem;
}

void heap_free( void *start, void *block )
{
  register uint32_t command asm ( "r0" ) = 3;
  register void *heap asm ( "r1" ) = start;
  register void *mem asm ( "r2" ) = block;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OS_Heap), "r" (command), "r" (heap), "r" (mem)
    : "lr", "cc" );
}

void setup_system_heap()
{
  uint32_t size = (&system_heap_top - &system_heap_base);

  if (0 != (size & 0xfff)) PANIC;

  memory_mapping map_system_heap = {
    .base_page = claim_contiguous_memory( size >> 12 ),
    .pages = size >> 12,
    .vap = &system_heap_base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &map_system_heap );

#ifdef DEBUG__POLLUTE_HEAPS
  // Real hardware doesn't start up with zeroed memory, this
  // might highlight some uninitialised variables bugs.
  for (uint32_t* p = (void*) &system_heap_base;
       ((uint8_t*) p) < &system_heap_top; p++) *p = 0xccddccdd;
#endif

  // OS_Heap won't run until shared.legacy.owner has been set, but that's
  // a word in a heap...
  // This is far from ideal, but....
  // TODO: Is this still needed, since early initialisation check?
  struct heap {
    uint32_t magic;
    uint32_t free;      // Offset
    uint32_t base;      // Offset
    uint32_t end;       // Offset
  };
  struct heap *sys = (struct heap *) &system_heap_base;
  sys->magic = 0x70616548;
  sys->free = 0;
  sys->base = sizeof( *sys );
  sys->end = &system_heap_top - &system_heap_base;
}

void setup_shared_heap()
{
  uint32_t size = (&shared_heap_top - &shared_heap_base);

  if (0 != (size & 0xfff)) PANIC;

  memory_mapping map_shared_heap = {
    .base_page = claim_contiguous_memory( size >> 12 ),
    .pages = size >> 12,
    .vap = &shared_heap_base,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &map_shared_heap );

#ifdef DEBUG__POLLUTE_HEAPS
  for (uint32_t* p = (void*) &shared_heap_base;
       ((uint8_t*) p) < &shared_heap_top; p++) *p = 0x55555555;
#endif

  // OS_Heap won't run until shared.legacy.owner has been set, but that's
  // a word in a heap...
  // This is far from ideal, but....
  struct heap {
    uint32_t magic;
    uint32_t free;      // Offset
    uint32_t base;      // Offset
    uint32_t end;       // Offset
  };

  struct heap *srd = (struct heap *) &shared_heap_base;
  srd->magic = 0x70616548;
  srd->free = 0;
  srd->base = sizeof( *srd );
  srd->end = &shared_heap_top - &shared_heap_base;
}

void setup_MOS_workspace()
{
  // Includes workspace for GSInit, etc. fa645800
  uint32_t size = 0x100000;
  extern uint8_t MOSworkspace;

  memory_mapping map = {
    .base_page = claim_contiguous_memory( size >> 12 ),
    .pages = size >> 12,
    .vap = &MOSworkspace,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &map );
}

// Shared block of memory that's (user?) rw (x?)
// Up to 1MiB size, and MiB aligned (for SharedCLib).
extern legacy_stack_frame legacy_svc_stack_top;

void setup_legacy_svc_stack()
{
  uint32_t top = (uint32_t) &legacy_svc_stack_top;
  uint32_t base = (top - 1) & ~0xfffff; // on MiB boundary
  // -1 in case whole MiB is used
  // e.g. top 0xff100000 -> 0xff0fffff -> base 0xff000000

  uint32_t pages = (top - base + 0xfff) >> 12;

  memory_mapping map = {
    .base_page = claim_contiguous_memory( pages ),
    .pages = pages,
    .va = base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };

  map_memory( &map );
}

extern LegacyZeroPage legacy_zero_page;

void IMB_Range()
{
  // Does this processor have split instruction and data caches?
  // I don't think so.
  // DSB, DMB, ISB?
  asm ( "dsb" );
  asm ( "dmb" );
  asm ( "isb" );
}

void setup_legacy_zero_page()
{
  // One block of memory shared by all cores. (At the moment.)
  uint32_t base = (uint32_t) &legacy_zero_page;
  uint32_t pages = (sizeof( legacy_zero_page ) + 0xfff) >> 12;

  memory_mapping map = {
    .base_page = claim_contiguous_memory( pages ),
    .pages = pages,
    .va = base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };

  map_memory( &map );

  memset( &legacy_zero_page, 0, sizeof( legacy_zero_page ) );
}

void *cb_array( int num, int size )
{
  uint32_t container_size = size + 4; // Object + size and free flag
  uint32_t alloc_size = num * container_size + sizeof( cba_head );

  cba_head *result = system_heap_allocate( alloc_size );
  if (result == (void*) 0xffffffff) return 0;

  // FIXME: Remove this; it's to try to trace a bug visible on real
  // hardware that accesses memory at 0x55555555 (uninitialised RAM)
  uint32_t *p = (void *) result;
  for (int i = 0; i < alloc_size / 4; i++) {
    p[i] = 0x77665544;
  }

  // Word following the cba_head
  uint32_t *blocks = &(result[1].total);

  result->total = num;
  result->container_size = container_size;
  result->first_free = blocks;

  uint32_t offset = 0;
  uint32_t words = container_size / 4;
  for (int i = 0; i < num; i++) {
#ifdef DEBUG__LOG_CB_ARRAYS
  if (i < 5) {
  Task_LogHexP( &blocks[i * words] );
  Task_LogString( " ", 1 );
  Task_LogHex( offset );
  Task_LogString( " ", 1 );
  Task_LogHexP( (&blocks[(i + 1) * words]) );
  Task_LogNewLine();
  }
#endif
    blocks[i * words] = 0x80000000 | offset;
    // Address of next block
    blocks[i * words + 1] = (uint32_t) (&blocks[(i + 1) * words]);
    offset += container_size;
  }
  uint32_t last = num - 1;
  blocks[last * words + 1] = 0;

#ifdef DEBUG__LOG_CB_ARRAYS
  Task_LogString( "Created CB array of ", 0 );
  Task_LogSmallNumber( num );
  Task_LogString( " entries of ", 0 );
  Task_LogSmallNumber( size );
  Task_LogString( " bytes at ", 0 );
  Task_LogHexP( result );
  Task_LogNewLine();
#endif

  return result;
}

void cba_free( cba_head *block, void *unwanted )
{
  uint32_t *bottom = &(block[1].total);
  uint32_t *top = bottom + (block->container_size * block->total) / 4;
  uint32_t *container = unwanted;
  container --;
  if (container >= bottom
   && container < top) {
    if (0 == (0x80000000 & *container)) {
      if (&bottom[(*container) / 4] == container) {
        *container |= 0x80000000;
        container[1] = (uint32_t) block->first_free;
        block->first_free = container;
      }
      else {
        PANIC; // Invalid unwanted, not the start of a container.
      }
    }
    else {
      PANIC; // Invalid unwanted, already free
    }
  }
  else {
    PANIC; // Should work, still untested
    system_heap_free( unwanted );
  }
}

void __attribute__(( noinline )) DoVduInit()
{
  extern void VduInit();
  register void *r12 asm( "r12" ) = &legacy_zero_page.vdu_drivers.ws;
  // Corrupts at least r4
  asm ( "bl VduInit" : : "r" (r12) : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr" );
}

void fill_legacy_zero_page()
{
  legacy_zero_page.Proc_IMB_Range = IMB_Range;

  legacy_zero_page.EscHan = 0xbad00000;

  legacy_zero_page.Page_Size = 4096;
  legacy_zero_page.OsbyteVars.LastBREAK = 1; // 1 Power on, 2 Control reset, 0 Soft

#define ZP_OFF( e ) ((uint8_t *)&legacy_zero_page.e - (uint8_t *) &legacy_zero_page)
#define CHECK_ZP_OFFSET( e, o ) assert( ZP_OFF( e ) == o )
#define CHECK_ZP_ALIGNED( e ) assert( (3 & ZP_OFF( e )) == 0 )
#define CHECK_ZP_ALIGNED16( e ) assert( (15 & ZP_OFF( e )) == 0 )

  CHECK_ZP_ALIGNED( OsbyteVars.SerialFlags );
  // Look for =ZeroPage+e in the source code, find the actual instruction
  // in the disassembly, and look at the word being loaded.
  // Or look for e.g. LDR R0, [R3, #BuffInPtrs]
  assert( 0xc4 == 0xa64 - 0x9a0 );
  assert( sizeof( legacy_zero_page.OsbyteVars ) == 0xc4 ); // 0xa64 - 0x9a0 );
  CHECK_ZP_OFFSET( OsbyteVars, 0x9a0 );
  CHECK_ZP_OFFSET( OsbyteVars.LastBREAK, 0x9f7 );
  CHECK_ZP_OFFSET( BuffInPtrs[0], 0xa64 );
  CHECK_ZP_OFFSET( EnvTime[0], 0xadc );

  extern vector_entry defaultvectab[];

  for (int i = 0; i < number_of( legacy_zero_page.VecPtrTab ); i++) {
    legacy_zero_page.VecPtrTab[i] = &defaultvectab[i];
  }

  DoVduInit();

  // Chocolate blocks (very similar to OSTask_pool, etc.)
  // Manually taken from Options and object sizes:
  // Callbacks
  legacy_zero_page.ChocolateCBBlocks = cb_array( 32, sizeof(callback_entry) );
  // Vectors
  legacy_zero_page.ChocolateSVBlocks = cb_array( 128, sizeof(vector_entry) );
  // Tickers
  legacy_zero_page.ChocolateTKBlocks = cb_array( 32, 20 );
  // I think I'm taking control of the functions these arrays support...
  // module ROM blocks
  // legacy_zero_page.ChocolateMRBlocks = cb_array( 150
  // module Active blocks
  // legacy_zero_page.ChocolateMABlocks = cb_array( 150
  // module SWI Hash blocks
  // legacy_zero_page.ChocolateMSBlocks = cb_array( 150
  legacy_zero_page.ChocolateMRBlocks = (void*) 0xbadbad01;
  legacy_zero_page.ChocolateMABlocks = (void*) 0xbadbad02;
  legacy_zero_page.ChocolateMSBlocks = (void*) 0xbadbad03;

  legacy_zero_page.OsbyteVars.Shadow = 1; // No shadow modes (0 turns on)

  // For OSCLI ; will need to be moved to a task and task slots, probably
  legacy_zero_page.OscliCBcurrend = 0;
  legacy_zero_page.OscliCBtopUID = 0;
  legacy_zero_page.OscliCBcurrend = 0xfa451800;

  // FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME
  legacy_zero_page.vdu_drivers.ws.ScreenStart = (void*) 0xc0000000;
  legacy_zero_page.vdu_drivers.ws.YWindLimit = 1080;
  legacy_zero_page.vdu_drivers.ws.LineLength = 1920 * 4;
  legacy_zero_page.vdu_drivers.ws.DisplayScreenStart = 0xc0000000;
  legacy_zero_page.vdu_drivers.ws.DisplayXWindLimit = 1919;
  legacy_zero_page.vdu_drivers.ws.DisplayYWindLimit = 1079;
  legacy_zero_page.vdu_drivers.ws.DisplayLog2BPP = 5;
  // FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME FIXME
}

// This routine is for SWIs implemented in the legacy kernel, 0-255, not in
// modules, in ROM or elsewhere. (i.e. routines that return using SLVK.)
void __attribute__(( noinline ))
run_riscos_code_implementing_swi( svc_registers *regs,
                                  uint32_t svc, uint32_t code )
{
  register uint32_t non_kernel_code asm( "r10" ) = code;
  register uint32_t swi asm( "r11" ) = svc;
  register svc_registers *regs_in asm( "r12" ) = regs;

  // Legacy kernel SWI functions expect the flags to be stored in lr
  // and the return address on the stack.

  asm volatile (
      "\n  push { r12 }"
      "\n  ldm r12, { r0-r9 }"
      "\n  adr lr, return_from_legacy_swi"
      "\n  push { lr } // return address, popped by SLVK"

      // Which SWIs use flags in r12 for input?
      "\n  ldr r12, [r12, %[spsr]]"
      "\n  bic lr, r12, #(1 << 28) // Clear V flags leaving original flags in r12"

      "\n  bx r10"
      "\nreturn_from_legacy_swi:"
      "\n  cpsid i // Disable interrupts"
      "\n  pop { r12 } // regs"
      "\n  stm r12, { r0-r9 }"
      "\n  ldr r0, [r12, %[spsr]]"
      "\n  bic r0, #0xf0000000"
      "\n  and r2, lr, #0xf0000000"
      "\n  orr r0, r0, r2"
      "\n  str r0, [r12, %[spsr]]"
      :
      : "r" (regs_in)
      , "r" (non_kernel_code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))
      , "r" (swi)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"
      , "r8", "r9", "lr", "memory" );
}

static inline
void switch_stacks( void *ftop, void *ttop )
{
  // Copy the stack above the current stack pointer to the new stack,
  // and set the stack pointer to the bottom of the new stack.

  // TODO: mov r2, sp\n mov sp, %[ttop]\n0:\n ldr r1, [%[ftop],#-4]!\n push {r1}\n cmp r2, %[ftop]\n bne 0b  - "r1", "r2" (or even use %[ttop] for r1 and "=r" (ttop))
  asm ( "mov r0, sp"
    "\n  mov sp, %[ttop]"
    "\n  mov r2, %[ftop]"
    "\n0:"
    "\n  ldr r1, [r2, #-4]!"
    "\n  push { r1 }"
    "\n  cmp r0, r2"
    "\n  bne 0b"
    :
    : [ftop] "r" (ftop)
    , [ttop] "r" (ttop)
    : "r0", "r1", "r2", "memory" );
}

OSTask *legacy_do_OS_Module( svc_registers *regs )
{
  uint32_t entry = JTABLE[OS_Module];
  run_riscos_code_implementing_swi( regs, OS_Module, entry );
  return 0;
}

// If no other subsystem wants to handle modules, use the legacy code
// (Which won't work!)
__attribute__(( weak ))
OSTask *do_OS_Module( svc_registers *regs )
{
  return legacy_do_OS_Module( regs );
}

__attribute__(( weak ))
OSTask *do_OS_ServiceCall( svc_registers *regs )
{
  return 0;
}

OSTask *do_OS_PlatformFeatures( svc_registers *regs )
{
  switch (regs->r[0]) {
  case 0:
    regs->r[0] = 0x80107ff9; // Taken from a running Rpi3
    regs->r[1] = 0;
    break;
  case 34:
    // https://www.riscosopen.org/wiki/documentation/show/OS_PlatformFeatures%2034
    // Flags copied from RPi3...
    switch (regs->r[1]) {
    case 1 ... 12:
    case 14:
    case 16 ... 21:
    case 23 ... 25:
    case 28 ... 38:
    case 41 ... 46:
    case 49 ... 52:
    case 54 ... 57:
    case 59:
      regs->r[0] = 1; break;
    case 0:
    case 13:
    case 15:
    case 22:
    case 26:
    case 27:
    case 39:
    case 40:
    case 47:
    case 48:
    case 53:
    case 58:
      regs->r[0] = 0; break;
    default:
      regs->r[0] = -1; break; // Unknown flag
    }
    break;
  default:
    regs->r[0] = 0;
    regs->r[1] = 0xbadf00d;
    PANIC;
  }
  return 0;
}

OSTask *do_OS_ReadSysInfo( svc_registers *regs )
{
  switch (regs->r[0]) {
  case 0: // Read the configured screen size in bytes
    regs->r[0] = 8 << 20; // FIXME FIXME FIXME
    break;
  case 8:
    regs->r[0] = 11; // New class of platform (CKernel)
    // Flags will include alignment options, but allow that to be
    // changed per slot. TODO
    regs->r[1] = 0;
    regs->r[2] = 0;
    break;
  case 1:
    {
      // 32 bpp -> 5 Log2BPP
      static uint32_t const msb[] = { 1, 1920, 1080, 5, 60, -1 };

      regs->r[0] = (uint32_t) msb;
      regs->r[1] = 7; // Use MDF
      regs->r[2] = 0;
    }
    break;
  case 6: // Some values are just plain dangerous
    {
      if (regs->r[1] == 0 && regs->r[2] == 18) {
        // Used by TaskWindow, but it then tries to write to it.
        // TaskWindow needs replacing.
        regs->r[2] = (uint32_t) &JTABLE;
        break;
      }
      else if (regs->r[1] == 0 && regs->r[2] == 16) {
        Task_LogString( "ReadSysInfo 6: top of SVC stack\n", 32 );
        regs->r[2] = (uint32_t) &legacy_svc_stack_top;
        // This might be worth a panic!
        break;
      }
      else if (regs->r[1] == 0 && regs->r[2] == 69) {
        Task_LogString( "ReadSysInfo 6: Address of IRQsema\n", 34 );
        regs->r[2] = (uint32_t) &legacy_zero_page.IRQsema;
        break;
      }
      else if (regs->r[1] == 0 && regs->r[2] == 70) {
        Task_LogString( "ReadSysInfo 6: Address of DomainId\n", 35 );
        regs->r[2] = (uint32_t) &legacy_zero_page.DomainId;
        break;
      }
      else if (regs->r[1] == 0 && regs->r[2] == 79) {
        Task_LogString( "ReadSysInfo 6: RISCOSLibWord\n", 29 );
        regs->r[2] = (uint32_t) &legacy_zero_page.RISCOSLibWord;
        break;
      }
      else if (regs->r[1] == 0 && regs->r[2] == 80) {
        Task_LogString( "ReadSysInfo 6: CLibWord\n", 24 );
        regs->r[2] = (uint32_t) &legacy_zero_page.CLibWord;
        break;
      }
      else {
        Task_LogString( "ReadSysInfo 6: ", 15 );
        Task_LogSmallNumber( regs->r[2] );
        Task_LogNewLine();
      }
    }
    // drop though
  default:
    {
      uint32_t entry = JTABLE[OS_ReadSysInfo];
      run_riscos_code_implementing_swi( regs, OS_ReadSysInfo, entry );
      break;
    }
  }
  return 0;
}

__attribute__(( weak ))
OSTask *run_module_swi( svc_registers *regs, int swi )
{
  PANIC;
  return 0;
}

__attribute__(( weak ))
bool handler_available( uint32_t swi )
{
  return false;
}

static inline
bool needs_legacy_stack( uint32_t swi )
{
  int32_t legacy[] = {
    0b11111111111111111111111111111111,         // 0-31 (0 on left!)
    0b11111111111111111111111111111111,         // 32-63
    0b11111111111111111111111111111111,         // 64-95
    0b11111111111111111111111111111111,         // 96-127
    0b11111111111111111111111111111111,         // 128-159
    0b11111111111111111111111111111111,         // 160-191
    0b11111111111111110000000000000000,         // 192-223
    0b00000000111111111111111111111111 };       // 224-255

  switch (swi) {
  case 0x000 ... 0x0ff: 
    // Shift the n-th from left bit to the top of the word
    return ((legacy[swi / 32]) << (swi % 32)) < 0;
  case 0x100 ... 0x1ff:
    return true; // OS_WriteI+
  case OSTask_Yield ... OSTask_Yield + 63:
    return false;
  }

  return !handler_available( swi );
}

static
void do_convert( svc_registers *regs, int swi )
{
  uint32_t val = regs->r[0];
  char *buf = (void*) regs->r[1];
  char *p = buf;
  uint32_t len = regs->r[2];
  char c;
  bool started = false;

  regs->r[0] = regs->r[1];

  static char const hex[16] = "0123456789ABCDEF";
  switch (swi) {
  case 0xd4:
    *p++ = hex[(val >> 28) & 0xf];
    *p++ = hex[(val >> 24) & 0xf];
    len -= 2;
  case 0xd3:
    *p++ = hex[(val >> 20) & 0xf];
    *p++ = hex[(val >> 16) & 0xf];
    len -= 2;
  case 0xd2:
    *p++ = hex[(val >> 12) & 0xf];
    *p++ = hex[(val >> 8) & 0xf];
    len -= 2;
  case 0xd1:
    *p++ = hex[(val >> 4) & 0xf];
    len -= 1;
  case 0xd0:
    *p++ = hex[(val >> 0) & 0xf];
    len -= 1;
    regs->r[2] = len;
    *p = '\0';
    regs->r[1] = (uint32_t) p;
    break;

  case 0xd5: val = val & 0xff;
  case 0xd6: val = val & 0xffff;
  case 0xd7: val = val & 0xffffff;
  case 0xd8:
    c = '0';
    while (val >= 1000000000) {
      c++;
      val -= 1000000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 100000000) {
      c++;
      val -= 100000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 10000000) {
      c++;
      val -= 10000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 1000000) {
      c++;
      val -= 1000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 100000) {
      c++;
      val -= 100000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 10000) {
      c++;
      val -= 10000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 1000) {
      c++;
      val -= 1000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 100) {
      c++;
      val -= 100;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 10) {
      c++;
      val -= 10;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    *p++ = '0' + val;
    regs->r[1] = (uint32_t) p;
    regs->r[2] -= (p - buf);
    *p = '\0';
    break;
    
  case 0xd9: val = (val << 24) >> 24;
  case 0xda: val = (val << 16) >> 16;
  case 0xdb: val = (val << 8) >> 8;
  case 0xdc:
    if ((int32_t) val < 0) { *p++ = '-'; val = -val; }
    c = '0';
    while (val >= 1000000000) {
      c++;
      val -= 1000000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 100000000) {
      c++;
      val -= 100000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 10000000) {
      c++;
      val -= 10000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 1000000) {
      c++;
      val -= 1000000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 100000) {
      c++;
      val -= 100000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 10000) {
      c++;
      val -= 10000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 1000) {
      c++;
      val -= 1000;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 100) {
      c++;
      val -= 100;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    c = '0';
    while (val >= 10) {
      c++;
      val -= 10;
    }
    if (started || c != '0') { *p++ = c; started = true; }
    *p++ = '0' + val;
    regs->r[1] = (uint32_t) p;
    regs->r[2] -= (p - buf);
    *p = '\0';
    break;
    
  default: PANIC;
  }
}

// Code to recover after an interrupt while in SVC mode.
void __attribute__(( naked, noreturn )) ResumeLegacy();

// Marker to indicate the task is a legacy one when being resumed.
// (Resumption is carried out in the execute_swi function.)
void __attribute__(( naked, noreturn )) BlockedLegacy()
{
  PANIC; // Needs some code to avoid being the same location as ResumeLegacy
}

struct legacy_stack_frame {
  legacy_stack_frame *up;
  OSTask *caller;
  uint32_t blocked_swi_lr;
  void *blocked_sp;
};

static svc_registers *const std_top = (void*) ((&workspace.svc_stack)+1);

//static
OSTask *run_the_swi( svc_registers *regs, uint32_t number )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = 0;
  uint32_t swi = (number & ~Xbit);

  bool is_legacy_task = (shared.legacy.frame != 0)
                     && (running == shared.legacy.frame->caller);

  switch (swi) {
  case 0xd0 ... 0xe8:
    {
      do_convert( regs, swi );
    }
    break;
  case OS_Module:
    {
      resume = do_OS_Module( regs );

      asm ( "cpsid i" );

      if (resume == 0 && Vset( regs )) {
        error_block const *err = (void*) regs->r[0];
        if (err->code == 0xff000000) {
          char const text[] = "Resetting SVC stack and entering module at ";
          Task_LogString( text, sizeof( text )-1 );
          Task_LogHex( regs->r[1] );
          Task_LogString( ", ", 2 );
          Task_LogHex( regs->r[2] );
          Task_LogNewLine();

          running->regs.lr = regs->r[1];
          running->regs.r[12] = regs->r[2];
          running->regs.spsr = 0x10;

          assert( is_legacy_task );
          assert( shared.legacy.frame != 0 );
          assert( shared.legacy.frame->caller == running );
          // Should resume the next frame up, if it's runnable? TODO

          shared.legacy.frame = shared.legacy.frame->up;
          running->banked_sp_usr = 0;
          running->banked_lr_usr = (uint32_t) unexpected_task_return;

          running->saved = run_the_swi;

          OSTask *owner = shared.legacy.owner;
          mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, owner );
          asm ( "udf 1" );

          // Passing running to ensure banked registers set
          return_to_swi_caller( running, &running->regs, std_top );
        }
      }
    }
    break;

  case OS_ServiceCall: do_OS_ServiceCall( regs ); break;

  // memory.c
  case OS_ReadDynamicArea: do_OS_ReadDynamicArea( regs ); break;
  case OS_DynamicArea: do_OS_DynamicArea( regs ); break;
  case OS_ChangeDynamicArea: do_OS_ChangeDynamicArea( regs ); break;
  case OS_Memory: do_OS_Memory( regs ); break;
  case OS_ValidateAddress: do_OS_ValidateAddress( regs ); break;
  case OS_AMBControl: do_OS_AMBControl( regs ); break;

  case OS_PlatformFeatures: do_OS_PlatformFeatures( regs ); break;
  case OS_ReadSysInfo: do_OS_ReadSysInfo( regs ); break;

  case OS_GetEnv:
    {
      // Leaving the warning below to highlight the TODO
      regs->r[0] = (uint32_t) "This should be the command";
      regs->r[1] = 0x400000; // FIXME
      regs->r[2] = 200; // FIXME
    }
    break;
  case OS_ReadMemMapInfo:
    {
      regs->r[0] = 4096; // Page size
      regs->r[1] = (1 << 20); // Lie: 1GiB
    }
    break;

  case OS_SynchroniseCodeAreas: break;
  case OS_SWINumberFromString:
    {
      // TODO in Modules
      static error_block const error = { 292, "SWI name not known" };

      regs->r[0] = (uint32_t) &error;
      regs->spsr |= VF;
    }
    break;

  case OS_SetCallBack:
    // Legacy's legacy. I think this is depricated in favour of
    // AddCallBack, but it's still used by at least the Wimp.
    legacy_zero_page.CallBack_Flag |= 1;
    break;

  case OS_Word:
    {
      if (regs->r[0] == 7) {
        // Beep! FIXME when sound stuff working
      }
      else {
        uint32_t entry = JTABLE[OS_Word];
        run_riscos_code_implementing_swi( regs, OS_Word, entry );
      }
    }
    break;
#ifdef DEBUG__FAKE_OS_BYTE
  case OS_Byte:
    {
      if (regs->r[0] == 0xa1 && regs->r[1] == 0x8c) {
        // 8 = Homerton.Medium
        // 12 = Trinity.Medium
        regs->r[2] = (2 * 12) + 1; // 3D look. Fixed font
      }
      else if (regs->r[0] == 0xa1 && regs->r[1] == 134) { // FontSize (in pages)
        regs->r[2] = 32;
      }
      else if (regs->r[0] >= 0x7c && regs->r[0] <= 0x7e) { // Esc condition
        regs->r[1] = 0; // No escape condition (if ack)
      }
      else if (regs->r[0] == 0x8f) {
        do_OS_ServiceCall( regs );
      }
      else {
        uint32_t entry = JTABLE[OS_Byte];
        run_riscos_code_implementing_swi( regs, OS_Byte, entry );
      }
    }
    break;
#endif

  case OSTask_Yield ... OSTask_Yield + 63:
    {
      resume = ostask_svc( regs, number );
    }
    break;

#ifdef DEBUG__LOG_COMMANDS
  case OS_CLI:
    {
      char const *cmd = (char*) regs->r[0];
      uint32_t len = 0;
      while (cmd[len] >= ' ') len++;
      Task_LogString( "OSCLI: ", 7 );
      Task_LogString( cmd, len );
      Task_LogNewLine();
#ifdef DEBUG__NOPOINTER
      if (cmd[0] == 'P' && cmd[1] == 'o') { // Pointer
        break; // FIXME FIXME FIXME FIXME FIXME FIXME FIXME
      }
#endif
      // DROPPING THROUGH!
    }
#endif

  default:
    {
      if (swi < OS_ConvertStandardDateAndTime) {
        uint32_t entry = JTABLE[swi];
        run_riscos_code_implementing_swi( regs, swi, entry );

#ifdef DEBUG__CHECK_CALLBACKS
        if (swi == OS_AddCallBack) {
          if (0 == legacy_zero_page.CallBack_Vector) PANIC;
        }
#endif
      }
      else if (swi < 256) { // OS_WriteI
        // Conversion SWIs
        extern void despatchConvert();
        uint32_t code = (uint32_t) despatchConvert;
        run_riscos_code_implementing_swi( regs, swi, code );
      }
      else if (swi < 512) { // OS_WriteI+
        uint32_t entry = JTABLE[OS_WriteC];

        uint32_t r0 = regs->r[0];
        regs->r[0] = swi & 0xff;
        run_riscos_code_implementing_swi( regs, 0, entry );
        if (Vclear( regs ))
          regs->r[0] = r0;
      }
      else {
        resume = run_module_swi( regs, swi );
      }
    }
  }

  // Some legacy SWIs enable interrupts and don't disable them again on
  // return.
  asm ( "cpsid i" );

#ifdef DEBUG__SHOW_ALL_ERRORS
  if (Vset( regs )) {
    static char const text[] = "Vset: ";
    Task_LogString( text, sizeof( text )-1 );
    error_block *error = (void*) regs->r[0];
    Task_LogHexP( error );
    if (error == 0) {
      static char const text[] = " NULL error";
      Task_LogString( text, sizeof( text )-1 );
    }
    else {
      Task_LogString( " ", 1 );
      Task_LogSmallNumber( error->code );
      Task_LogString( " ", 1 );
      Task_LogString( error->desc, 0 );
    }
    Task_LogNewLine();
  }
#endif

  return resume;
}

__attribute__(( noreturn, noinline ))
void execute_swi( svc_registers *regs, int number )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = 0;
#if 0
  if ((number & ~Xbit) < 0x2c0 || (number & ~Xbit) > 0x300) {
    OSTask *t = running;
    do {
      Task_LogHexP( t );
      t = t->next;
    } while (t != running);
    Task_LogNewLine();
  }
#endif
#if 0
#ifdef DEBUG__SHOW_LEGACY_SWIS
  if ((number & ~Xbit) != 0x2db) {
      if (0) { char const text[] = "SWI ";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHex( number );
      Task_LogString( " in ", 4 );
      Task_LogHexP( running );
#if 0
      Task_LogString( " ", 1 );
      Task_LogHexP( shared.legacy.frame );
      { char const text[] = " -> ";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHexP( regs+1 );
#endif
      Task_LogNewLine();
  }
#endif
#endif

  if (running == shared.legacy.owner
   && number == (OS_CallASWIR12 | Xbit)) {
    // The caller has been de-queued by the user mode code, with its
    // banked_.._usr values stored in the task.

    // The user mode code is only unblocked if there is no legacy SWI
    // running or the latest legacy SWI has been blocked.

    // That means either:
    //   No task was using the legacy svc stack
    // or
    //   The task that was using it has been blocked (but may have
    //   already been resumed by another core).

    // Legacy stack frame (apart from whatever the compiler chooses):
    //   svc_registers
    //   old blocked sp
    //   old legacy_task

    // Disable the owner task until the stack is free for another task
    // to use it.
    resume = stop_running_task( regs );

    OSTask *legacy_task = ostask_from_handle( regs->r[0] );
    uint32_t legacy_swi = regs->r[1];

    assert( legacy_task->controller[0] == running );
    // user code doesn't do anything, we deal with ownership here.
    pop_controller( legacy_task );

    if (0xffffff == legacy_swi) {
      // Not a real SWI; the task has become runnable, but the owner
      // task has to be stopped from listening when it resumes.
      legacy_stack_frame *f = shared.legacy.frame;
      assert( f != 0 );
      while (f != 0 && f->caller != legacy_task) {
        f = f->up;
      }
      assert( f != 0 );
      legacy_task->regs.lr = f->blocked_swi_lr;
      // Indicates task can be resumed when this frame becomes the
      // current frame (all later legacy SWIs have completed):
      f->blocked_swi_lr = 0;

#ifdef DEBUG__LOG_LEGACY_STACK
      { char const text[] = "Task ";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHex( ostask_handle( running ) );
      if (f == shared.legacy.frame) {
        char const text[] = " running again";
        Task_LogString( text, sizeof( text )-1 );
      }
      else {
        char const text[] = ", but not owner of stack";
        Task_LogString( text, sizeof( text )-1 );
      }
      Task_LogNewLine();
#endif

      if (f == shared.legacy.frame) {
        assert( !legacy_task->running );

        dll_attach_OSTask( legacy_task, &workspace.ostask.running );

        put_usr_registers( legacy_task );

        return_to_swi_caller( legacy_task, &legacy_task->regs, f->blocked_sp );
      }
      else {
        assert( !resume->running );

        put_usr_registers( resume );

        return_to_swi_caller( resume, &resume->regs, std_top );
      }
    }

#ifdef DEBUG__LOG_LEGACY_STACK
    { char const text[] = "New lagacy stack owner ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( legacy_task ) );
    Task_LogNewLine();
#endif

    // Give the task whose handle is in r0 the legacy stack and
    // run the SWI (which may be interrupted).

    dll_attach_OSTask( legacy_task, &workspace.ostask.running );

    // We've changed tasks, but when we re-call this funtion it will
    // look like we haven't, so this has to be done here, it won't
    // happen in return_to_swi_caller.
    put_usr_registers( legacy_task );
    legacy_task->running = 1;

    // The shared.legacy.owner task is now detached. It will be resumed
    // when another task is permitted to run a legacy SWI, either:
    // when this SWI is complete and the legacy_task resumes after the SWI.
    // or
    // when this legacy task is blocked by program action (i.e. waiting for
    // something like data from a pipe) (when resume is non-zero when running
    // is the legacy_task).

    // HOWEVER! The task that was blocked could have been picked up by another
    // core and be running again already.

    // A new frame gets put on the stack when a new task gets control of the
    // legacy stack.

    // We no longer need the registers, so use the top of them to hold the
    // legacy_stack_frame.

    legacy_stack_frame *top = &legacy_svc_stack_top;
    if (shared.legacy.frame != 0) {
      top = shared.legacy.frame->blocked_sp;
    }

    legacy_stack_frame *f = top-1;

    f->up = shared.legacy.frame;
    f->caller = legacy_task;
    // blocked_swi_lr will be filled in around the call to run_the_swi
    shared.legacy.frame = f;
    // Done in this order so caller is initialised before the frame is
    // visible.

    svc_registers *legacy_regs = ((svc_registers *) f) - 1;
    *legacy_regs = legacy_task->regs;

    register svc_registers *r asm( "r0" ) = legacy_regs;
    register uint32_t s asm( "r1" ) = legacy_swi;
    asm ( "mov sp, r0"
      "\n  b execute_swi"
      :
      : "r" (r)
      , "r" (s)
      , [execute_swi] "m" (execute_swi) );
    __builtin_unreachable();
  }

  // Either running is the task that owns the legacy stack, or the SWI
  // does not need that stack.

  if ((number & ~Xbit) == OS_CallASWIR12) {
    number = regs->r[12];
  }
  else if ((number & ~Xbit) == OS_CallASWI) {
    number = regs->r[10];
  }

  uint32_t swi = (number & ~Xbit);
  bool generate_error = (number == swi);
  bool is_legacy_task = (shared.legacy.frame != 0)
                     && (running == shared.legacy.frame->caller);

  // TODO: XOS_CallASWI( Xwhatever ) is clear, but
  // OS_CallASWI( Xwhatever ) or XOS_CallASWI( whatever ) is less so.

  if (swi == OS_CallASWIR12 || swi == OS_CallASWI) {
    PANIC; // I think maybe the legacy implementation loops forever...
  }

  // Special case for startup.
  // Minor (valid) assumption: the few early SWIs don't use much stack, the
  // core stack will be enough and there won't be any interrupts anyway.

  if (shared.legacy.owner != 0  // Not early on in initialisation
   && !is_legacy_task
   && needs_legacy_stack( swi )) {
    // Not owner of legacy stack, but needs it? Wait for it.
#ifdef DEBUG__TRACK_LEGACY_TASKS
    { char const text[] = "Queuing task for legacy stack ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( running ) );
    { char const text[] = ", SWI is ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( number );
    if (shared.legacy.frame != 0) {
      char const text[] = ", legacy task is ";
      Task_LogString( text, sizeof( text )-1 );
      Task_LogHexP( shared.legacy.frame->caller );
    }
    Task_LogNewLine();
#endif
    resume = queue_running_OSTask( regs, shared.legacy.queue, number );

    assert( resume != 0 );
  }
  else if (is_legacy_task) {
    // Important: If the SWI runs without getting blocked, it will stay
    // on the same core (resume will be null).

    assert( shared.legacy.frame != 0 );
    uint32_t lr = regs->lr;

    shared.legacy.frame->blocked_swi_lr = lr;
    shared.legacy.frame->blocked_sp = regs + 1;

    // In case the SWI blocks the task, this will mark it as special.
    // BlockedLegacy never actually gets executed.
    regs->lr = (uint32_t) BlockedLegacy;

    if (OS_WriteS == (number & ~Xbit)) {
      // Damned special case!
      uint32_t r0 = regs->r[0];
      regs->r[0] = lr;

      resume = run_the_swi( regs, (number & Xbit) | OS_Write0 );
      assert( resume == 0 ); // If it does block, it will be in the WriteC

      lr = (regs->r[0] + 3) & ~3; // Align

      if (Vclear( regs )) regs->r[0] = r0;
    }
    else 
      resume = run_the_swi( regs, number );

    if (resume == 0) {
      // The task wasn't blocked.
      regs->lr = lr;
    }
    else {
#ifdef DEBUG__SHOW_LEGACY_SWIS
      { char const text[] = "Legacy task blocked ";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHexP( running );
      Task_LogString( " ", 1 );
      Task_LogHexP( shared.legacy.frame );
      { char const text[] = " -> ";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHexP( regs+1 );
      Task_LogNewLine();
#endif
      // A legacy SWI task has been blocked, listen for legacy SWIs again.
      // (Also listening for tasks becoming active again.)
      OSTask *owner = shared.legacy.owner;
      mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, owner );
    }
  }
  else {
    // The SWI doesn't need the legacy stack

    resume = run_the_swi( regs, number );
  }

  assert( (resume == 0) || (resume != running) );
  assert( (resume != 0) == (resume == workspace.ostask.running) );

  if (resume == 0
   && generate_error
   && Vset( regs )) {
    // FIXME: TENTATIVE
Task_LogString( "Legacy error: ", 14 );
Task_LogHex( swi );
Task_LogString( "/", 1 );
Task_LogHex( number );
Task_LogString( " ", 1 );
Task_LogString( (char*) regs->r[0] + 4, 0 );
Task_LogNewLine();
    uint32_t entry = JTABLE[OS_CallAVector];
    regs->r[9] = 1; // ErrorV
    // FIXME: Why aren't I calling OS_GenerateError here?
    run_riscos_code_implementing_swi( regs, OS_CallAVector, entry );
        asm ( "udf #0x101" );
  }

  if (resume == 0 && (void*) (regs+1) == (void*) shared.legacy.frame) {
    // Dropping back to usr32 from a legacy SWI, make use of the
    // legacy SVC stack while we have it!

#ifdef DEBUG__TRACK_LEGACY_TASKS
    { char const text[] = "End of legacy SWI ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHexP( running );
    { char const text[] = ", SWI was ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( swi );
    Task_LogNewLine();
#endif
    if (0 == (regs->spsr & 0x80)) { // With interrupts enabled
      asm ( "udf 8" );
      if ((legacy_zero_page.CallBack_Flag & 1) != 0
       && swi != OS_SetCallBack) {
        // Call Callback handler
        // Note, this seems to be the legacy's legacy callback approach!
        // No longer used by Wimp to set R2 on exit from Wimp_Poll if
        // event = 18, UserMessageRecorded
        PANIC;
      }

      // Postponed? There shouldn't be anywhere doing this!
      // Maybe it went: SetCallBack sets bit 0, on return, since it
      // shouldn't call the callback then (why not?), it gets moved to
      // bit 1, and the handler actually gets called then?
      if ((legacy_zero_page.CallBack_Flag & 2) == 2) PANIC;

      if ((legacy_zero_page.CallBack_Flag & 6) == 4) {
        // Run callbacks, release callback_entries
        // FIXME There are multiple conditions that should occur before
        // running this code that I don't fully understand.
        // Returning from an interrupt to user mode should also call them,
        // that needs some thought.
        // Or elimination of modules that handle interrupts from legacy code.
        callback_entry *entry = legacy_zero_page.CallBack_Vector;
        while (entry != 0) {
          callback_entry run = *entry;
          asm ( "udf 9" );
#ifdef DEBUG__SHOW_CALLBACKS
  Task_LogString( "Callback ", 9 );
  Task_LogHex( run.code );
  Task_LogNewLine();
#endif
          legacy_zero_page.CallBack_Vector = entry->next;
          cba_free( legacy_zero_page.ChocolateCBBlocks, entry );
          register uint32_t workspace asm ( "r12" ) = run.workspace;
          register uint32_t code asm ( "r14" ) = run.code;
          asm ( "udf #0x100" );
          asm volatile ( "blx lr" : "=r" (code)
                                  : "r" (workspace)
                                  , "r" (code)
                                  : "memory" );
          // Some legacy SWIs enable interrupts and don't disable them
          // again on return. Probably some callbacks, too.
          asm ( "cpsid ai" );

          // You would expect this to be run.next, but what if a callback
          // adds a callback?
          entry = legacy_zero_page.CallBack_Vector;
        }
      }
    }
#ifdef DEBUG__TRACK_LEGACY_TASKS
    { char const text[] = "Dropping back to usr32 from a legacy SWI ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHexP( running );
    { char const text[] = ", SWI is ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( swi );
    Task_LogNewLine();
#endif

    // Detached owner
    assert( shared.legacy.owner->next == shared.legacy.owner );
    assert( shared.legacy.frame != 0 );

    shared.legacy.frame = shared.legacy.frame->up;
    if (shared.legacy.frame != 0
     && shared.legacy.frame->blocked_swi_lr == 0) {
      // It's been resumed
      OSTask *caller = shared.legacy.frame->caller;
      assert( !caller->running );
      mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, caller );
    }
    else {
      // We can accept a new task calling a legacy SWI (or the current
      // stack owner being resumed, if there is one).
      OSTask *owner = shared.legacy.owner;
      assert( !owner->running );
      mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, owner );
    }

    asm ( "udf 2" );
    // Carry on the legacy task
    return_to_swi_caller( 0, regs, std_top );
  }

  if (resume == 0) {
    // Stick with whatever stack we're on
    asm ( "udf 6" );
    return_to_swi_caller( 0, regs, regs+1 );
  }
  else {
    // running has been programatically blocked.
    // It may also have been resumed by a SWI on another core.

    assert( resume != running );
    assert( resume == workspace.ostask.running );
    // assert( running->running ); Invalid: state has been saved
    assert( !running->running ); // state has been saved
    assert( !resume->running );

    if (resume->regs.lr == (uint32_t) BlockedLegacy) {
#ifdef DEBUG__SHOW_LEGACY_SWIS
      { char const text[] = "Legacy task released ";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHexP( resume );
      Task_LogString( " ", 1 );
      Task_LogHex( resume->banked_sp_usr );
      Task_LogString( " ", 1 );
      Task_LogHexP( shared.legacy.frame );
      Task_LogNewLine();
#endif

      // To keep queue_running_OSTask/stop_running_task happy
      resume->running = 1;
      put_usr_registers( resume ); // FIXME: may fix error but needs cleaner solution!
 
      OSTask *old = resume;
      assert( old->running );
      resume = queue_running_OSTask( &resume->regs,
                                     shared.legacy.queue,
                                     0xffffff );
      assert( !old->running );
      assert( !resume->running );

      assert( workspace.ostask.running == resume );
    }

    // Can a second running task be legacy blocked?
    // I don't think so.
    // A released task will be either the next task in the running
    // list for this core (returned by stop_running_task) or the
    // queue's handler which shouldn't be a legacy task.
    assert( resume->regs.lr != (uint32_t) BlockedLegacy );

    if (resume->regs.lr == (uint32_t) ResumeLegacy) {
      // Return from interrupt, but into SVC
#ifdef DEBUG__SHOW_LEGACY_SWIS__breaks
      // I think this breaks the system because the task in running
      // is not yet running...
      { static char const text[] = "RFI";
      Task_LogString( text, sizeof( text )-1 ); }
      Task_LogHexP( resume );
      Task_LogNewLine();
#endif
      put_usr_registers( resume );
      assert( resume != 0 );
    }

    return_to_swi_caller( resume, &resume->regs, std_top );
  }
}

static void make_desktop_workspace()
{
  uint32_t const pages = 1; // FIXME?
  memory_mapping map = {
    .base_page = claim_contiguous_memory( pages ),
    .pages = pages,
    .va = 0xff000000,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 }; // FIXME?

  map_memory( &map );
}

static void make_sprite_extend_workspace()
{
  uint32_t const pages = 1; // FIXME?
  memory_mapping map = {
    .base_page = claim_contiguous_memory( pages ),
    .pages = pages,
    .va = 0xfaff3000,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 }; // FIXME?

  map_memory( &map );
}

static void do_TickOnes( uint32_t handle, uint32_t pipe )
{
  // No actual data is being transmitted, it's simply a mechanism
  // to buffer a number of ticks so TickOne gets called the correct
  // number of times.
  // TickOne will be called in a privileged mode with interrupts
  // disabled.
  for (;;) {
    PipeSpace data = PipeOp_WaitForData( pipe, 1 );
    while (data.available != 0) {
      data = PipeOp_DataConsumed( pipe, 1 );
      extern void TickOne(); // Corrupts r12
      register void *ip asm ( "r12" ) = &legacy_zero_page.OsbyteVars;
      asm volatile ( "bl TickOne"
             : "=r" (ip)
             : [tick] "m" (TickOne)
             , "r" (ip)
             : "r0", "r1", "r2", "r3", "lr", "cc" );
    }
  }
}

void __attribute__(( noreturn )) centiseconds()
{
  // This task wakes up every centisecond and triggers a call to TickerV
  // etc. from another task (do_TickOnes).

  // This implementation is a bit hacky (unnecessarily so), but I just
  // can't help myself! The buffer is never read or written as such, it's
  // just a limited number of bytes. So, I've used the stack memory as
  // both pipe and (temporarily) registers. Just to save a few bytes of
  // RMA. Sad, isn't it?
  svc_registers regs;

  uint32_t pipe = PipeOp_CreateOnBuffer( (void*) &regs, sizeof( regs ) );

  uint32_t const tick_one_stack_size = 240;
  uint32_t base = (uint32_t) shared_heap_allocate( tick_one_stack_size );
  uint32_t stack = ~7 & (base + tick_one_stack_size);

  register uint32_t sp asm ( "r13" );
  if (sp < 0x20000000) asm ( "bkpt 55" );

  uint32_t cs = Task_CreateService1( do_TickOnes, stack, pipe );
  assert( cs != 0 );
  PipeOp_SetReceiver( pipe, cs );
  Task_GetRegisters( cs, &regs );
                        // The task needs to be able to access "zero page"
                        // It should use this task's stack, though
  // FWIW, no OS with a system call OS_EnterOS can reasonably claim to have
  // any security, so making everything writable by everything wouldn't
  // really make much difference, assuming programs are well-written.
  // Just saying: when there's no 32-bit privileged modes, maybe just
  // loosening things up when running solid programs (while having a route
  // to write secure programs) will be the way to go.
  // See also code variables.

  regs.spsr = 0x9f; // System32 mode, interrupts disabled
  Task_ReleaseTask( cs, &regs );

  PipeSpace space = PipeOp_WaitForSpace( pipe, 1 );

  for (;;) {
    Task_Sleep( 9 ); // Returns every 10th tick.
    if (space.available != 0) {
      space = PipeOp_SpaceFilled( pipe, 1 );
    }
    else {
      char text[] = "Skipped a centisecond tick!\n";
      Task_LogString( text, sizeof( text ) - 1 );
    }
  }
}

void __attribute__(( noreturn )) startup()
{
  // Running with multi-tasking enabled. This routine gets called
  // just once.

  uint32_t queue = Task_QueueCreate();

  shared.legacy.queue = queue;

  uint32_t handle = Task_SpawnTask1( serve_legacy_swis, 0, queue );
  assert( shared.legacy.owner == 0 );

  setup_legacy_svc_stack();
  setup_legacy_zero_page();

  setup_system_heap(); // System heap
  setup_shared_heap(); // RMA heap
  setup_MOS_workspace(); // Hopefully soon to be removed

  uint32_t const tick_stack_size = 256; // regs, buffer & a bit more
  uint32_t base = ~7 & (uint32_t) shared_heap_allocate( tick_stack_size );
  uint32_t cs = Task_CreateTask0( centiseconds, tick_stack_size + base );
  assert( cs != 0 );

  fill_legacy_zero_page();

  make_desktop_workspace();
  make_sprite_extend_workspace(); // FIXME!! IDK why this is here!

  // No more called_during_initialisation in execute_swi
  shared.legacy.owner = ostask_from_handle( handle );
  assert( shared.legacy.owner != 0 );

  asm ( "mov sp, %[reset_sp]"
    "\n  cpsie aif, #0x10"
    :
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  for (int i = 0; i <= 16; i++) {
    register int handler asm ( "r0" ) = i;
    asm ( "svc %[def]"
      "\n  svc %[set]"
      :
      : [def] "i" (OS_ReadDefaultHandler)
      , [set] "i" (OS_ChangeEnvironment)
      , "r" (handler)
      : "r1", "r2", "r3" );
  }

  extern char const build_script[];
  extern char const build_options[];
  extern char const modcflags[];

  Task_LogString( build_script, 0 );
  Task_LogNewLine();
  Task_LogString( build_options, 0 );
  Task_LogNewLine();
  Task_LogString( modcflags, 0 );
  Task_LogNewLine();

  // RMRun HAL
  register uint32_t run asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules.HAL";

  asm ( "svc %[swi]" : : [swi] "i" (OS_Module), "r" (run), "r" (module) );

  PANIC;

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) ResumeLegacy()
{
  // When interrupted task resumes, restore sp, the cpsr, then lr, and
  // the pc.

  // We know that the I flag was clear when the task was interrupted,
  // because otherwise it wouldn't have been interrupted!

  register void **legacy_sp asm( "lr" ) = &shared.legacy.sp;
  asm ( "ldr sp, [lr]"

    "\n  push { r0 }"           // Now 4 words on the stack
    "\n  ldr r0, [sp, #4]"      // old_legacy_sp
    "\n  str r0, [lr]"
    "\n  pop { r0, lr }" // Don't care about the lr value, but adds 8 to sp
    "\n  cpsie i"
    "\n  pop { lr, pc }"
    :
    : "r" (legacy_sp) );
}

void interrupting_privileged_code( OSTask *task )
{
  // This should only ever happen in legacy code.
  // We need to be able to get the task back to the state it was in when
  // interrupted, without corrupting anything.
  uint32_t svc_lr;
  uint32_t old_legacy_sp = (uint32_t) shared.legacy.sp;
  asm ( "mrs %[sp], sp_svc"
    "\n  mrs %[lr], lr_svc"
    "\n  msr sp_svc, %[reset_sp]"
    : [sp] "=&r" (shared.legacy.sp)
    , [lr] "=&r" (svc_lr)
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  // TODO: Check here if the legacy stack is exhausted. If it is, there's
  // probably an interrupt that's stuck on incorrectly, I think.

  uint32_t *stack = shared.legacy.sp;
  stack -= 3;
  shared.legacy.sp = stack;
  stack[0] = old_legacy_sp;
  stack[1] = svc_lr;
  stack[2] = task->regs.lr;

  // Assumption: no more than 1 MiB allocated to legacy svc stack
  // Required by legacy code anyway.
  uint32_t legacy_base = ((uint32_t) &legacy_svc_stack_top - 1) >> 20;
  if ((((uint32_t) shared.legacy.sp) >> 20) != legacy_base) {
    { char const text[] = "Interrupted SVC ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHexP( shared.legacy.sp );
    Task_LogString( " ", 1 );
    Task_LogHex( old_legacy_sp );
    Task_LogString( " ", 1 );
    Task_LogHex( svc_lr );
    Task_LogString( " ", 1 );
    Task_LogHex( task->regs.lr );
    Task_LogNewLine();
  }

  // Now, when the task resumes, make it run our code to restore the
  // privileged state.
  task->regs.lr = (uint32_t) ResumeLegacy;
  // Don't let the ResumeLegacy routine be interrupted while that's happening!
  task->regs.spsr |= 0x80;
}
