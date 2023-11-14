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
#include "queues.h"

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

extern uint8_t legacy_zero_page;

void setup_legacy_zero_page()
{
  uint32_t base = (uint32_t) &legacy_zero_page;
  uint32_t pages = 4; // 16KiB

  memory_mapping map = {
    .base_page = claim_contiguous_memory( pages ),
    .pages = pages,
    .va = base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };

  map_memory( &map );

  memset( &legacy_zero_page, 0, pages << 12 );
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

void run_kernel_swi( svc_registers *regs, int swi )
{
  uint32_t legacy_top = (uint32_t) &legacy_svc_stack_top;
  uint32_t legacy_base = (legacy_top - 1) & ~0xfffff;
  uint32_t std_top = (uint32_t) (&workspace.svc_stack+1);

  // It will be this or the legacy stack. So far, just this...
  if ((uint32_t)(regs + 1) != std_top) PANIC;

  OSTask *running = workspace.ostask.running;
  uint32_t handle = ostask_handle( running );

  // This comparison is mp-safe because a word access is atomic and
  // no other core will ever set it to this task's handle.
  if (shared.legacy.owner != handle) {
    if (0 == change_word_if_equal( &shared.legacy.owner, 0, handle )) {
      // Just gained the legacy stack!
      // Duplicate the core's stack onto it so we can return safely

      asm ( "mov r0, sp"
        "\n  mov sp, %[top]"
        "\n0:"
        "\n  ldr r1, [%[ctop], #-4]!"
        "\n  push { r1 }"
        "\n  cmp r0, %[ctop]"
        "\n  bne 0b"
        :
        : [top] "r" (legacy_top)
        , [ctop] "r" (std_top)
        : "r0", "r1" );

      if ((uint32_t)(regs + 1) != std_top) PANIC;
      regs = (void*) (((uint32_t) regs) - std_top + legacy_top);
      if ((uint32_t)(regs + 1) != legacy_top) PANIC;
    }
    else {
      // Not owner of legacy stack, wait for it.
      queue_running_OSTask( regs, shared.legacy.queue, swi );
      return;
    }
  }
  switch (swi) {
  case 0 ... 127:
    {
      uint32_t entry = shared.legacy.jtable[swi];
      if ((entry & 3) == 0) {
        run_risos_code_implementing_swi( regs, swi, entry );
      }
      else PANIC; // Queue
    }
    break;
  default: PANIC;
  }

  uint32_t sp;
  asm ( "mov %[sp], sp" : [sp] "=r" (sp) );
  if ((sp >> 20) != (legacy_base >> 20)) PANIC;

  if ((uint32_t)(regs + 1) != legacy_top) PANIC;

  if ((uint32_t)(regs + 1) == legacy_top) {
    // Run callbacks
  }
}

extern void run_module_swi( svc_registers *regs, int swi );

void execute_swi( svc_registers *regs, int number )
{
  int swi = number & ~Xbit;

  // OSTask SWIs have already been filtered out
  if (swi >= 0x100 && swi < 0x200) {
    // WriteI
    // Translate to OS_WriteC with number & 0xff
    uint32_t r0 = regs->r[0];
    regs->r[0] = (swi & 0xff);
    run_kernel_swi( regs, 0 );
    regs->r[0] = r0;
  }
  else {
    switch (swi) {
    case 0 ... 128:
      run_kernel_swi( regs, swi );
      break;
    default:
      PANIC;
    }

    if ((number & Xbit) != 0 && (regs->spsr & VF) != 0) {
      PANIC; // call OS_GenerateError
    }
  }
}

void spawn_legacy_manager( uint32_t pipe )
{
  register void *start asm( "r0" ) = serve_legacy_swis;
  register void *sp asm( "r1" ) = 0;
  register uint32_t r1 asm( "r2" ) = pipe;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void __attribute__(( noreturn )) startup()
{
  // Running with multi-tasking enabled. This routine gets called
  // just once.

  enable_page_level_mapping();

  // Illicit knowlege of mmu implementation
  if (shared.mmu.free == 0) PANIC;

  setup_system_heap(); // System heap
  setup_shared_heap(); // "RMA" heap

  setup_legacy_svc_stack();
  setup_legacy_zero_page(); // FIXME: universal, or per task or slot?

  extern uint32_t JTABLE[128];

  for (int i = 0; i < number_of( shared.legacy.jtable ); i++) {
    shared.legacy.jtable[i] = JTABLE[i];
  }

  register uint32_t handle asm( "r0" );
  asm ( "svc %[swi]"
    : "=r" (handle)
    : [swi] "i" (OSTask_QueueCreate)
    : "lr" );

  shared.legacy.queue = handle;

  spawn_legacy_manager( handle );

  svc_pre_boot_sequence();

  asm ( "mov sp, %[reset_sp]"
    "\n  cpsie aif, #0x10"
    :
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  boot_sequence();
}

