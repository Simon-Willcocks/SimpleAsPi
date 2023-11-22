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

void setup_system_heap()
{
  extern uint8_t system_heap_base;
  extern uint8_t system_heap_top;

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

  heap_initialise( &system_heap_base, size );
}

void setup_shared_heap()
{
  // Shared heap of memory that's user rwx
  extern uint8_t shared_heap_base;
  extern uint8_t shared_heap_top;

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

  heap_initialise( &shared_heap_base, size );
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

  memory_mapping map = {
    .base_page = claim_contiguous_memory( 256 ),
    .pages = (top - base) >> 12,
    .va = base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };

  map_memory( &map );
}

extern LegacyZeroPage legacy_zero_page;

static void __attribute__(( naked )) end_vector()
{
  asm ( "pop {pc}" );
}

static vector_entry const vector_end = { .workspace = 0, .code = (uint32_t) end_vector, .next = 0 };

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

  for (int i = 0; i < number_of( legacy_zero_page.VecPtrTab ); i++) {
    legacy_zero_page.VecPtrTab[i] = &vector_end;
  }

}

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

// SWIs from usr32 mode all go through the queue and the server task.
// Any SWIs they call will be run directly, on the legacy stack.
// On return from the initial SWI, it will run any callbacks and then
// release the legacy stack.
// Returning from that SWI will return to the run_swi routine which
// resumes the server task.
bool swi_is_legacy()
{
  return true;
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
  int swi = number & ~Xbit;
  bool generate_error = (number == swi);

  // TODO: XOS_CallASWI( Xwhatever ) is clear, but
  // OS_CallASWI( Xwhatever ) or XOS_CallASWI( whatever ) is less so.

  if (swi == OS_CallASWIR12) {
    swi = regs->r[12];
  }
  else if (swi == OS_CallASWI) {
    swi = regs->r[10];
  }
  if (swi == OS_CallASWIR12
   || swi == OS_CallASWI) {
    PANIC; // I think the legacy implementation loops forever...
  }

  uint32_t legacy_top = (uint32_t) &legacy_svc_stack_top;
  uint32_t std_top = (uint32_t) (&workspace.svc_stack+1);
  svc_registers *legacy_regs = regs;

  OSTask *running = workspace.ostask.running;
  uint32_t handle = ostask_handle( running );

  bool new_owner = !in_legacy_stack();

  if (new_owner && (uint32_t)(regs + 1) != std_top) PANIC;

  // This comparison is mp-safe because a word access is atomic and
  // no other core will ever set it to this task's handle because they're
  // not running this task.
  if (*shared.legacy.owner == handle) {
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
      do_OS_Module( legacy_regs );
      }
      break;
    default:
      if (swi < 128) {
        extern uint32_t JTABLE[128];
        uint32_t entry = JTABLE[swi];
        asm ( "b 1f\n1:" : : "r" (entry) );
        run_risos_code_implementing_swi( legacy_regs, swi, entry );
      }
      else run_module_swi( legacy_regs, swi );
    }

    if (regs != legacy_regs) {
      *regs = *legacy_regs;
    }

    if (generate_error && (regs->spsr & VF) != 0) {
      PANIC; // Call OS_GenerateError!
    }

    if (new_owner) {
      // Will be returning to the server task now.

      // TODO: Run callbacks

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

void spawn_legacy_manager( uint32_t pipe, uint32_t *owner )
{
  register void *start asm( "r0" ) = serve_legacy_swis;
  register void *sp asm( "r1" ) = 0;
  register uint32_t r1 asm( "r2" ) = pipe;
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

  enable_page_level_mapping();

  setup_system_heap(); // System heap
  setup_shared_heap(); // "RMA" heap

  setup_legacy_svc_stack();
  setup_legacy_zero_page(); // FIXME: universal, or per task or slot?

  uint32_t handle = Task_QueueCreate();

  shared.legacy.queue = handle;
  shared.legacy.owner = shared_heap_allocate( 4 );

  spawn_legacy_manager( handle, shared.legacy.owner );

  svc_pre_boot_sequence();

  asm ( "mov sp, %[reset_sp]"
    "\n  cpsie aif, #0x10"
    :
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  for (register int i asm ( "r0" ) = 0; i <= 16; i++) {
    asm ( "svc %[def]"
      "\n  svc %[set]"
      :
      : [def] "i" (OS_ReadDefaultHandler)
      , [set] "i" (OS_ChangeEnvironment)
      , "r" (i)
      : "r1", "r2", "r3" );
  }

  boot_sequence();
}

void __attribute__(( naked, noreturn )) ResumeLegacy()
{
  // When interrupted task resumes, that will restore sp, lr, and the pc.
  register uint32_t **legacy_sp asm( "lr" ) = &shared.legacy.sp;
  asm ( "ldr sp, [lr]"
    "\n  pop { lr, pc }"
    :
    : "r" (legacy_sp) );
}

void interrupting_privileged_code( OSTask *task )
{
  uint32_t svc_lr;
  asm ( "mrs %[sp], sp_svc"
    "\n  mrs %[lr], lr_svc"
    "\n  msr sp_svc, %[reset_sp]"
    : [sp] "=&r" (shared.legacy.sp)
    , [lr] "=&r" (svc_lr)
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  shared.legacy.sp -= 2;
  shared.legacy.sp[0] = svc_lr;
  shared.legacy.sp[1] = task->regs.lr;
  task->regs.lr = (uint32_t) ResumeLegacy;
}
