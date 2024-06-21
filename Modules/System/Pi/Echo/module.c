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

// QA7 module, for use on Raspberry Pi 2 - 3

#include "CK_types.h"
#include "ostaskops.h"
#include "bcm_uart.h"

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

const char title[] = "Echo";
const char help[] = "Echo serial\t0.01 (" CREATION_DATE ")";

UART volatile *const uart  = (void*) 0x7000;

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

void echo()
{
  for (;;) {
    // Should wait for interrupt from GPU, etc.
    Task_Sleep( 20 );
    uart->data = 'U';
  }
}

void start_echo( uint32_t handle )
{
  // This should be passed to the final driver module by the HAL
  uint32_t uart_page = 0x3f201000 >> 12;
  Task_MapDevicePages( (uint32_t) uart, uart_page, 1 );

  echo();
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  uint32_t const stack_size = 256;
  uint8_t *stack = rma_claim( stack_size );

  register void *start asm( "r0" ) = start_echo;
  register void *sp asm( "r1" ) = aligned_stack( stack + stack_size );
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    : "lr", "cc" );
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


