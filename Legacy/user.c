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

static struct locals {
  svc_registers regs;
  uint32_t sp; // May be corrupted by OS_CallASWI call
  uint32_t registers[12];
} *locals = (void*) 0x8000;

#define regs locals->regs

void manage_legacy_stack( uint32_t handle, uint32_t queue )
{
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
    Task_Yield();
#endif

    bool writeS = ((client.swi & ~Xbit) == OS_WriteS);
    // This SWI is unique (I hope), in changing the return address
    uint32_t r0 = regs.r[0];
    if (writeS) regs.r[0] = regs.lr;

    uint32_t swi = writeS ? OS_Write0 : client.swi;

    regs.spsr &= 0x0fffffff;

    // This gets a bit ugly.
    // RISC OS thinks it's a good idea for OS code to be interruptable.
    // I don't, so most of this kernel doesn't allow it.
    // When an interrupt occurs, the handler always stores the current
    // usr_SP, usr_LR for the currently running task; a reasonable
    // action considering only usr mode code should be interruptable.
    // Unfortunately for the legacy handling, that will be this task,
    // but the usr context can be from a totally different task.
    // That's not a problem until the legacy swi finishes and returns
    // to this usr code with an invalid sp and lr.
    // To avoid this problem, the simplest approach I can think of is
    // simply to use storage at a fixed address in this slot to
    // maintain a consistent context.
    register uint32_t s asm( "r12" ) = swi | Xbit;
    register uint32_t c asm( "r10" ) = client.task_handle;
    asm ( "mov r14, #0x8000"
      "\n  add r14, %[sp]"
      "\n  str sp, [r14], #4"
      "\n  stm r14, {r0-r11}"
      "\n  mov r14, #0x8000"
      "\n  ldm r14, {r0-r9}"
      "\n  mov r11, r0"
      "\n  mov r0, r10"
      "\n  svc %[enter_slot]" // No access to locals now
      "\n  mov r0, r11"
      "\n  svc %[swi]"
      "\n  mrs r10, cpsr  // Save cpsr"
      "\n  svc %[leave_slot]"
      // Back in our slot!
      "\n  mov r14, #0x8000"
      "\n  stm r14, {r0-r9}"
      "\n  ldr r1, [r14, %[spsr]]"
      "\n  and r10, r10, #0xf0000000"
      "\n  bic r1, r1, #0xf0000000"
      "\n  orr r1, r1, r10"
      "\n  str r1, [r14, %[spsr]]"
      "\n  add r14, %[sp]"
      "\n  ldr sp, [r14], #4"
      "\n  ldm r14, {r0-r11}"
      :
      : [swi] "i" (OS_CallASWIR12 | Xbit)
      , [enter_slot] "i" (OSTask_RunForTask | Xbit)
      , [leave_slot] "i" (OSTask_Finished | Xbit)
      , [sp] "i" (offset_of( struct locals, sp ))
      , "r" (s)
      , "r" (c)
      , [spsr] "i" (offset_of( svc_registers, spsr ))
      : "memory", "cc", "r14" );

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
        Task_LogHex( error->code );
        Task_LogString( " ", 1 );
        Task_LogString( error->desc, 0 );
        Task_LogString( " ", 1 );
        Task_LogHex( client.swi );
        Task_LogString( " ", 1 );
        Task_LogHex( lr );
        Task_LogNewLine();
#ifdef DEBUG__BREAK_ON_REPORTABLE_ERROR
        Task_Sleep( 2 );
        asm ( "bkpt 7" );
#endif
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
