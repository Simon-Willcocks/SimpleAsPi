/* Copyright 2021 Simon Willcocks
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
#include "ostaskops.h"

typedef struct workspace workspace;

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "Pipes";
const char help[] = "Pipes test\t0.01 (" CREATION_DATE ")";

void play( uint32_t handle, uint32_t pipe_in, uint32_t pipe_out )
{
  for (;;) {
    PipeSpace data = PipeOp_WaitForData( pipe_in, 4 );
    if (data.available != 4) asm ( "bkpt 1" );
    uint32_t v = *(uint32_t*) data.location;
    PipeOp_DataConsumed( pipe_in, 4 );

    PipeSpace space = PipeOp_WaitForSpace( pipe_out, 4 );
    if (space.available < 4) asm ( "bkpt 1" );
    *(uint32_t*) space.location = v + 1;
    PipeOp_SpaceFilled( pipe_out, 4 );
  }
}

void starter( uint32_t handle, uint32_t pipe_out, uint32_t pipe_in )
{
  // Throw in the ball...
  PipeSpace space = PipeOp_WaitForSpace( pipe_out, 4 );
  if (space.available < 4) asm ( "bkpt 1" );
  *(uint32_t*) space.location = 0x77000000;
  PipeOp_SpaceFilled( pipe_out, 4 );

  play( handle, pipe_in, pipe_out ); // Note: switched order
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  uint32_t a = PipeOp_CreateForTransfer( 4096 );
  uint32_t b = PipeOp_CreateForTransfer( 4096 );

  PipeOp_SetSender( a, 0 );
  PipeOp_SetReceiver( a, 0 );

#if 0
  // Let's make this really simple...
  // We'll be the sender and the receiver.
  // This test works, but the task is never blocked.

  uint32_t v = 0x88000000;
  for (;;) {
    PipeSpace space = PipeOp_WaitForSpace( a, 4 );
    if (space.available < 4) asm ( "bkpt 1" );
    *(uint32_t*) space.location = v + 1;
    PipeOp_SpaceFilled( a, 4 );

    PipeSpace data = PipeOp_WaitForData( a, 4 );
    if (data.available != 4) asm ( "bkpt 1" );
    v = *(uint32_t*) data.location;
    PipeOp_DataConsumed( a, 4 );
  }
#endif

  PipeOp_SetSender( b, 0 );
  PipeOp_SetReceiver( b, 0 );

  uint32_t const stack_size = 256;
  uint8_t *stack = rma_claim( stack_size * 2 );
{
  register void *start asm( "r0" ) = starter;
  register void *sp asm( "r1" ) = aligned_stack( stack + stack_size );
  register uint32_t r1 asm( "r2" ) = a;
  register uint32_t r2 asm( "r3" ) = b;
  register uint32_t handle asm( "r0" );
  asm volatile (
        "svc %[swi_create]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (sp)
    , "=r" (handle)
    : [swi_create] "i" (OSTask_Create)
    , [swi_release] "i" (OSTask_Release_Task)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}
{
  register void *start asm( "r0" ) = play;
  register void *sp asm( "r1" ) = aligned_stack( stack + 2 * stack_size );
  register uint32_t r1 asm( "r2" ) = a;
  register uint32_t r2 asm( "r3" ) = b;
  register uint32_t handle asm( "r0" );
  asm volatile (
        "svc %[swi_create]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (sp)
    , "=r" (handle)
    : [swi_create] "i" (OSTask_Create)
    , [swi_release] "i" (OSTask_Release_Task)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}
}

void __attribute__(( naked )) init()
{
  register struct workspace **private asm ( "r12" );
  register char const *env asm ( "r10" );
  register uint32_t instantiation asm ( "r11" );

  // Move r12 into argument register
  asm volatile ( "push { lr }" );

  c_init( private, env, instantiation );

  asm ( "pop { pc }" );
}

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}


