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
#include "kernel_swis.h"
#include "ostaskops.h"

static void __attribute__(( naked )) run_swi( uint32_t task, svc_registers *regs )
{
  asm ( "svc %[swi]"
    "\n  mov r12, r0"
    "\n  mov r0, r11"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_RelinquishControl)
    , [swi] "i" (OS_CallASWIR12 | Xbit) );

  // The above should never return.
  asm ( "udf #0" );
}

void manage_legacy_stack( uint32_t handle, uint32_t pipe, uint32_t *owner )
{
  svc_registers regs;

  for (;;) {
    queued_task task = Task_QueueWait( pipe );

    Task_GetRegisters( task.task_handle, &regs );

    uint32_t r11 = regs.r[11];
    uint32_t r12 = regs.r[12];
    uint32_t lr = regs.lr;

    regs.r[11] = handle;
    regs.r[12] = task.swi;
    regs.lr = (uint32_t) run_swi;

    *owner = task.task_handle;
    Task_RunThisForMe( task.task_handle, &regs );
    *owner = 0;

    Task_GetRegisters( task.task_handle, &regs );

    regs.r[0] = regs.r[12];
    regs.r[11] = r11;
    regs.r[12] = r12;
    regs.lr = lr;

    Task_ReleaseTask( task.task_handle, &regs );
  }
}

void __attribute__(( naked, noreturn )) serve_legacy_swis( uint32_t handle,
                                                           uint32_t pipe,
                                                           uint32_t *owner )
{
  // Running in usr32 mode, no stack, but my very own slot
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0", "r1" );

  manage_legacy_stack( handle, pipe, owner );

  __builtin_unreachable();
}