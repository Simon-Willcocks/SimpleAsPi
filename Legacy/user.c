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

void manage_legacy_stack( uint32_t handle, uint32_t queue )
{
  svc_registers regs;

  for (;;) {
    queued_task client = Task_QueueWait( queue );

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

    uint32_t swi = writeS ? OS_Write0 : client.swi;

    regs.spsr &= 0x0fffffff;

    // DO NOT USE THE STACK BETWEEN RunForTask AND Finished!
    // Or create a stack in shared memory (RMA)

    register uint32_t s asm( "r12" ) = swi | Xbit;
    register svc_registers *r asm( "r14" ) = &regs;
    asm ( "push {r0-r11}"
      "\n  mov r10, %[client]"
      "\n  ldm r14, {r0-r9}"
      "\n  mov r11, r0"
      "\n  mov r0, r10"
      "\n  svc %[enter_slot]"
      "\n  mov r0, r11"
      "\n  svc %[swi]"
      "\n  mrs r10, cpsr  // Save cpsr"
      "\n  svc %[leave_slot]"
      // Back in our context, with our stack!
      "\n  stm r14, {r0-r9}"
      "\n  ldr r1, [r14, #4 * 14]" // spsr
      "\n  and r10, r10, #0xf0000000"
      "\n  bic r1, r1, #0xf0000000"
      "\n  orr r1, r1, r10"
      "\n  str r1, [r14, #4 * 14]" // spsr
      "\n  pop {r0-r11}"
      :
      : [swi] "i" (OS_CallASWIR12 | Xbit)
      , [enter_slot] "i" (OSTask_RunForTask | Xbit)
      , [leave_slot] "i" (OSTask_Finished | Xbit)
      , "r" (r)
      , "r" (s)
      , [client] "r" (client.task_handle)
      : "r10", "memory", "cc" );

    bool generate_errors = (0 == (client.swi & Xbit));

#ifdef DEBUG__SHOW_LEGACY_SWIS
    Task_LogString( "Legacy SWI ", 11 );
    Task_LogHex( client.swi );
    Task_LogString( " ended\n", 7 );
#endif

    if (0 != (VF & regs.spsr)) { // Error? Could be enter module request
      error_block const *error = (void*) regs.r[0];
      if (error->code == 0) {
        // Special case: we just successfully ran an OS_Module call to
        // enter a module either directly, or indirectly (what we called
        // called OS_Module from SVC mode).

        // Treat this as a call to return the start and private addresses and
        // run the code in usr32.

        regs.r[12] = regs.r[2];
        regs.lr = regs.r[1];
        regs.spsr = 0x10; // usr32, interrupts enabled
      }
      else if (generate_errors) {
        // TODO GenerateError
        asm ( "bkpt 7" );
      }
      else {
        asm ( "bkpt 8" );
      }
    }
    else {
      if (writeS) {
        asm( "bkpt 9" );
        regs.lr = (3 + regs.r[0]) & ~3; // R0 on exit from SWI (OS_WriteS)
        regs.r[0] = r0;   // Restored to state before OS_WriteS
      }
      else {
        regs.r[11] = r11;
        regs.lr = lr;
      }
      regs.r[12] = r12;
    }

    Task_ReleaseTask( client.task_handle, &regs );
  }
}

void __attribute__(( naked, noreturn )) serve_legacy_swis( uint32_t handle,
                                                           uint32_t queue )
{
  // Running in usr32 mode, no stack, but my very own slot
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0", "r1" );

  manage_legacy_stack( handle, queue );

  __builtin_unreachable();
}
