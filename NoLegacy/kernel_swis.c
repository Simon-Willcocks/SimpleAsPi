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

/* Allows modules to be run without any legacy support. */
/* Currently only new modules. */
/* Include subsystem SimpleHeap (or a later equivalent) */

#include "CK_types.h"
#include "ostask.h"
#include "qa7.h"
#include "bcm_gpio.h"
#include "bcm_uart.h"
#include "heap.h"
#include "raw_memory_manager.h"
#include "kernel_swis.h"

extern uint8_t system_heap_base;
extern uint8_t system_heap_top;
// Shared heap of memory that's user rwx
extern uint8_t shared_heap_base;
extern uint8_t shared_heap_top;

// No legacy modules.
uint32_t const LegacyModulesList = 0;

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

  heap_initialise( &system_heap_base, &system_heap_top - &system_heap_base );
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

  heap_initialise( &shared_heap_base, &shared_heap_top - &shared_heap_base );
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

// This NoLegacy subsystem only makes sense with the Modules subsystem
// that provides
OSTask *do_OS_Module( svc_registers *regs );
OSTask *run_module_swi( svc_registers *regs, int swi );
// bool needs_legacy_stack( uint32_t swi )

void execute_swi( svc_registers *regs, int number )
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

  switch (swi) {
  case OS_Module:
    {
    bool module_run = (regs->r[0] == 0 || regs->r[0] == 2);

    do_OS_Module( regs );

    if (module_run && 0 == (regs->spsr & VF)) {
      regs->r[12] = regs->r[1];
      regs->lr = regs->r[2];
    }

    break;

    }
  case OS_ServiceCall: // No-one is listening
    break;

  default:
    {
    OSTask *new_task = run_module_swi( regs, swi );

    if (new_task == 0) break;

    return_to_swi_caller( new_task, &new_task->regs, regs+1 );
    }
  }

  if (generate_error && 0 != (VF & regs->spsr)) {
    PANIC; // TODO
  }

  if (0 != (VF & regs->spsr)) {
    PANIC; // TODO
  }

  return_to_swi_caller( 0, regs, regs+1 );
}

void __attribute__(( noreturn )) startup()
{
  // Running with multi-tasking enabled. This routine gets called
  // just once.

  setup_system_heap(); // System heap
  setup_shared_heap(); // RMA heap

  asm ( "mov sp, %[reset_sp]"
    "\n  cpsie aif, #0x10"
    :
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  // RMRun HAL
  register uint32_t run asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules.HAL";

  asm ( "svc %[swi]" : : [swi] "i" (OS_Module), "r" (run), "r" (module) );

  PANIC;

  __builtin_unreachable();
}

