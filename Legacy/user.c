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
    "\n  mrs r11, cpsr  // Save cpsr"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_Finished)
    , [swi] "i" (OS_CallASWIR12 | Xbit) );

  // The above should never return.
  for (;;) asm ( "udf #99" );
}

void manage_legacy_stack( uint32_t handle, uint32_t pipe, uint32_t *owner )
{
  svc_registers regs;

  for (;;) {
    queued_task client = Task_QueueWait( pipe );

    Task_GetRegisters( client.task_handle, &regs );

    uint32_t r11 = regs.r[11];
    uint32_t r12 = regs.r[12];
    uint32_t lr = regs.lr;

#ifdef DEBUG__SHOW_LEGACY_SWIS
    Task_LogString( "Legacy SWI ", 11 );
    Task_LogHex( client.swi );
    Task_LogNewLine();
    for (int i = 0; i < 10; i++) {
      Task_LogHex( regs.r[i] );
      Task_LogString( i == 9 ? "\n" : " ", 1 );
    }
#endif

    bool writeS = ((client.swi & ~Xbit) == OS_WriteS);
    // This SWI is unique (I hope), in changing the return address
    uint32_t r0 = regs.r[0];
    if (writeS) regs.r[0] = regs.lr;

    regs.r[12] = writeS ? OS_Write0 : client.swi;
    regs.lr = (uint32_t) run_swi;

    *owner = client.task_handle;
    Task_RunThisForMe( client.task_handle, &regs );
    *owner = 0;

#ifdef DEBUG__SHOW_LEGACY_SWIS
    Task_LogString( "Legacy SWI ", 11 );
    Task_LogHex( client.swi );
    Task_LogString( " ended\n", 7 );
#endif

    Task_GetRegisters( client.task_handle, &regs );

    if (regs.r[12] != client.swi) {
      // Special case: we just successfully ran an OS_Module call to
      // enter a module. We reset the SVC stack and enter the code in
      // usr32 mode.

      // These register choices have to match do_OS_Module
      // regs.r[0] is the command line (not so much, at the moment!)
      // Oh, wait, this is an OS_ChangeEnvironment thing, isn't it?
      // Or OS_FSControl 2?
      regs.r[0] = regs.r[2];
      regs.r[12] = regs.r[3];
      regs.lr = regs.r[4];
      regs.spsr = 0x10; // usr32, interrupts enabled
    }
    else if (writeS) {
      regs.spsr = regs.r[11]; // State on exit from SWI
      regs.lr = (3 + regs.r[0]) & ~3; // R0 on exit from SWI (OS_WriteS)
      regs.r[0] = r0;   // Restored to state before OS_WriteS
      regs.r[12] = OS_WriteS;
    }
    else {
      regs.spsr = regs.r[11]; // State on exit from SWI
      regs.r[11] = r11;
      regs.r[12] = r12;
      regs.lr = lr;
    }

    Task_ReleaseTask( client.task_handle, &regs );
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
