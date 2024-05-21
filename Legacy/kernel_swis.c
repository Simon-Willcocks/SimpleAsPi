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
    .usr32_access = 0 };
  map_memory( &map_system_heap );

  // OS_Heap won't run until shared.legacy.owner has been set, but that's
  // a word in a heap...
  // This is far from ideal, but....
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

  memory_mapping map = {
    .base_page = claim_contiguous_memory( size >> 12 ),
    .pages = size >> 12,
    .va = 0xfa600000,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &map );
}

// Shared block of memory that's (user?) rw (x?)
// Up to 1MiB size, and MiB aligned (for SharedCLib).
extern uint8_t legacy_svc_stack_top;

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

  uint32_t offset = sizeof( cba_head );
  uint32_t words = container_size / 4;
  for (int i = 0; i < num; i++) {
#ifdef DEBUG__LOG_CB_ARRAYS
  if (i < 5) {
  Task_LogHex( (uint32_t) &blocks[i * words] );
  Task_LogString( " ", 1 );
  Task_LogHex( offset );
  Task_LogString( " ", 1 );
  Task_LogHex( (uint32_t) (&blocks[(i + 1) * words]) );
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
  Task_LogHex( (uint32_t) result );
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
      if (&bottom[(*container - sizeof( cba_head )) / 4] == container) {
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

uint32_t sizeofOsbyteVars = sizeof( legacy_zero_page.OsbyteVars );

void fill_legacy_zero_page()
{
  legacy_zero_page.Proc_IMB_Range = IMB_Range;

  legacy_zero_page.Page_Size = 4096;
  legacy_zero_page.OsbyteVars.LastBREAK = 1; // 1 Power on, 2 Control reset, 0 Soft

#define ZP_OFF( e ) ((uint8_t *)&legacy_zero_page.e - (uint8_t *) &legacy_zero_page)
#define CHECK_ZP_OFFSET( e, o ) assert( ZP_OFF( e ) == o )
#define CHECK_ZP_ALIGNED( e ) assert( (3 & ZP_OFF( e )) == 0 )
#define CHECK_ZP_ALIGNED16( e ) assert( (15 & ZP_OFF( e )) == 0 )

  CHECK_ZP_ALIGNED( OsbyteVars.SerialFlags );
  // Look for =ZeroPage+e in the source code, find the actual instruction in the
  // disassambly, and look at the word being loaded.
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
}

uint32_t ZPOFF_OsbyteVars = ZP_OFF( OsbyteVars );
uint32_t ZPOFF_BuffInPtrs = ZP_OFF( BuffInPtrs );
// This routine is for SWIs implemented in the legacy kernel, 0-255, not in
// modules, in ROM or elsewhere. (i.e. routines that return using SLVK.)
void __attribute__(( noinline ))
run_risos_code_implementing_swi( svc_registers *regs,
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

static void switch_stacks( uint32_t ftop, uint32_t ttop )
{
  // Copy the stack above the current stack pointer to the new stack,
  // and set the stack pointer to the bottom of the new stack.
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
    : "r0", "r1", "r2" );
}

static bool in_legacy_stack()
{
  uint32_t legacy_top = (uint32_t) &legacy_svc_stack_top;
  uint32_t legacy_base = (legacy_top - 1) & ~0xfffff;
  uint32_t sp_section;
  asm volatile ( "mov %[sp], sp, lsr#20" : [sp] "=r" (sp_section) );
  return sp_section == (legacy_base >> 20);
}

// If no other subsystem wants to handle modules, use the legacy code
__attribute__(( weak ))
OSTask *do_OS_Module( svc_registers *regs )
{
  extern uint32_t JTABLE[128];

  uint32_t entry = JTABLE[OS_Module];
  run_risos_code_implementing_swi( regs, OS_Module, entry );
  return 0;
}

__attribute__(( weak ))
OSTask *do_OS_ServiceCall( svc_registers *regs )
{
  return 0;
}

__attribute__(( weak ))
OSTask *run_module_swi( svc_registers *regs, int swi )
{
  PANIC;
  return 0;
}

__attribute__(( weak ))
bool needs_legacy_stack( uint32_t swi )
{
  return true;
}

OSTask *execute_swi( svc_registers *regs, int number )
{
  // TODO: XOS_CallASWI( Xwhatever ) is clear, but
  // OS_CallASWI( Xwhatever ) or XOS_CallASWI( whatever ) is less so.

  if ((number & ~Xbit) == OS_CallASWIR12) {
    number = regs->r[12];
  }
  else if ((number & ~Xbit) == OS_CallASWI) {
    number = regs->r[10];
  }

  int swi = number & ~Xbit;
  bool generate_error = (number == swi);

  if (swi == OS_CallASWIR12
   || swi == OS_CallASWI) {
    PANIC; // I think the legacy implementation loops forever...
  }
#ifdef DEBUG__SHOW_LEGACY_SWIS
  if (number != 0x1001) { // Not timer tick
  Task_LogString( "SWI: ", 5 );
  Task_LogHex( number );
  Task_LogNewLine();
  Task_LogHex( regs->r[0] );
  Task_LogString( " ", 1 );
  Task_LogHex( regs->r[1] );
  Task_LogString( " ", 1 );
  Task_LogHex( regs->r[2] );
  Task_LogString( " ", 1 );
  Task_LogHex( regs->r[3] );
  Task_LogNewLine();
  }
#endif

  // Assumes all kernel SWIs need the legacy stack FIXME
  if (!needs_legacy_stack( swi )) {
    return run_module_swi( regs, swi );
  }

  uint32_t legacy_top = (uint32_t) &legacy_svc_stack_top;
  uint32_t std_top = (uint32_t) (&workspace.svc_stack+1);
  svc_registers *legacy_regs = regs;

  OSTask *running = workspace.ostask.running;
  uint32_t handle = ostask_handle( running );

  // A new owner of the legacy stack will have to swap stacks.
  // Avoid that if there's no support task yet.
  bool new_owner = !in_legacy_stack() && shared.legacy.owner != 0;

  // This will usually be the case, but not in startup
  if (new_owner && (uint32_t)(regs + 1) != std_top) PANIC;

  // Special case for allocations made before shared.legacy.owner has been
  // set - does not need the legacy stack, nor the support task.
  // This comparison is mp-safe because a word access is atomic and
  // no other core will ever set it to this task's handle because they're
  // not running this task.
  if (shared.legacy.owner == 0 // Note: the pointer to a word in RMA
   || *shared.legacy.owner == handle) { // The word, itself

    if (new_owner) {
      // Duplicate the core's stack onto it so we can return safely

      switch_stacks( std_top, legacy_top );

      if ((uint32_t)(regs + 1) != std_top) PANIC;
      legacy_regs = (void*) (((uint32_t) regs) - std_top + legacy_top);
      if ((uint32_t)(legacy_regs + 1) != legacy_top) PANIC;
    }

    switch (swi) {
    case OS_Module:
      {
      bool module_run = (legacy_regs->r[0] == 0 || legacy_regs->r[0] == 2);
      if (module_run && !new_owner) PANIC;
      // FIXME: This returns an OSTask * (always 0, but...)
      do_OS_Module( legacy_regs );
      // If this call was successful, the serve_legacy_swis function in
      // Legacy/user.c will cause the module to start using the changed
      // register values.
      }
      break;
    case OS_ServiceCall: do_OS_ServiceCall( legacy_regs ); break;
    case OS_ReadDynamicArea: do_OS_ReadDynamicArea( legacy_regs ); break;
    case OS_DynamicArea: do_OS_DynamicArea( legacy_regs ); break;
    case OS_ChangeDynamicArea: do_OS_ChangeDynamicArea( legacy_regs ); break;
    case OS_Memory: do_OS_Memory( legacy_regs ); break;
    case OS_ValidateAddress: do_OS_ValidateAddress( legacy_regs ); break;
    // case OS_Heap: do_OS_Heap( legacy_regs ); break;
    default:
      {
        extern uint32_t JTABLE[128];
        if (swi < 128) {
          uint32_t entry = JTABLE[swi];
          run_risos_code_implementing_swi( legacy_regs, swi, entry );
        }
        else if (swi >= 256 && swi < 512) {
          uint32_t r0 = legacy_regs->r[0];
          uint32_t entry = JTABLE[0];

          legacy_regs->r[0] = swi & 0xff;
          run_risos_code_implementing_swi( legacy_regs, 0, entry );
          legacy_regs->r[0] = r0;
        }
        else if (swi < 512) { asm( "udf 1" : : "r" (regs->lr) ); PANIC; } // Conversion SWIs
        else run_module_swi( legacy_regs, swi );
      }
    }

    if (regs != legacy_regs) {
      *regs = *legacy_regs;
    }

    if (generate_error && (regs->spsr & VF) != 0) {
      // FIXME: TENTATIVE
      extern uint32_t JTABLE[128];
      uint32_t entry = JTABLE[OS_CallAVector];
      run_risos_code_implementing_swi( legacy_regs, OS_CallAVector, entry );
    }

    if (new_owner) {
      // Will be returning to the server task now.

      // I don't grok the postponed or oldstyle flags.
      if ((legacy_zero_page.CallBack_Flag & 2) == 2) PANIC;

      if ((legacy_zero_page.CallBack_Flag & 6) == 4) {
        // Run callbacks, release callback_entries
        // FIXME There are multiple conditions that should occur before
        // running this code that I don't fully understand.
        // Returning from an interrupt to user mode should also call them,
        // that needs some thought.
        callback_entry *entry = legacy_zero_page.CallBack_Vector;
        while (entry != 0) {
          callback_entry run = *entry;
          legacy_zero_page.CallBack_Vector = entry->next;
          cba_free( legacy_zero_page.ChocolateCBBlocks, entry );
          register uint32_t workspace asm ( "r12" ) = run.workspace;
          register uint32_t code asm ( "r14" ) = run.code;
          asm ( "blx lr" : : "r" (workspace), "r" (code) );

          // You would expect this to be run.next, but what if a callback
          // adds a callback?
          entry = legacy_zero_page.CallBack_Vector;
        }
      }

      switch_stacks( legacy_top, std_top );
    }
  }
  else {
    // Not owner of legacy stack, wait for it.
    return queue_running_OSTask( regs, shared.legacy.queue, swi );
    // Changed workspace.ostask.running
  }

  return 0;
}

static void spawn_legacy_manager( uint32_t queue, uint32_t *owner )
{
  register void *start asm( "r0" ) = serve_legacy_swis;
  register void *sp asm( "r1" ) = 0;
  register uint32_t r1 asm( "r2" ) = queue;
  register uint32_t *r2 asm( "r3" ) = owner;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}

void __attribute__(( noreturn )) startup()
{
  // Running with multi-tasking enabled. This routine gets called
  // just once.

  setup_legacy_svc_stack();
  setup_legacy_zero_page();

  setup_system_heap(); // System heap
  setup_shared_heap(); // RMA heap
  setup_MOS_workspace(); // Hopefully soon to be removed

  fill_legacy_zero_page(); // Before shared.legacy.owner set

  shared.legacy.owner = shared_heap_allocate( 4 );
  *shared.legacy.owner = 0; // No owner, to start with.

  uint32_t handle = Task_QueueCreate();

  shared.legacy.queue = handle;

  spawn_legacy_manager( handle, shared.legacy.owner );

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

  // RMRun HAL
  register uint32_t run asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules.HAL";

  asm ( "svc %[swi]" : : [swi] "i" (OS_Module), "r" (run), "r" (module) );

  PANIC;

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) ResumeLegacy()
{
  // When interrupted task resumes, that will restore sp, the cpsr, then lr,
  // and the pc.

  // We know that the I flag was clear when the task was interrupted, because
  // it wouldn't have been interrupted!

  register uint32_t **legacy_sp asm( "lr" ) = &shared.legacy.sp;
  asm ( "ldr sp, [lr]"

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
  asm ( "mrs %[sp], sp_svc"
    "\n  mrs %[lr], lr_svc"
    "\n  msr sp_svc, %[reset_sp]"
    : [sp] "=&r" (shared.legacy.sp)
    , [lr] "=&r" (svc_lr)
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  // TODO: Check here if the legacy stack is exhausted. If it is, there's
  // probably an interrupt that's stuck on incorrectly, I think.

  shared.legacy.sp -= 2;
  shared.legacy.sp[0] = svc_lr;
  shared.legacy.sp[1] = task->regs.lr;
  task->regs.lr = (uint32_t) ResumeLegacy;
  // Don't let the ResumeLegacy routine be interrupted!
  task->regs.spsr |= 0x80;
}
