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
#include "ostaskops.h"

typedef struct workspace workspace;

struct workspace {
  uint32_t stack[60];
};

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

//NO_start;
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

const char title[] = "DoNothing";
const char help[] = "DoNothing\t0.00 (" CREATION_DATE ")";

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
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

void __attribute__(( noreturn )) Nothing()
{
  {
  register uint32_t pin asm( "r0" ) = 27; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 200;
  register uint32_t off asm( "r2" ) = 100;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );
  }

  Task_Sleep( 850 );

  {
  register uint32_t pin asm( "r0" ) = 22; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 210;
  register uint32_t off asm( "r2" ) = 410;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );
  }

  for (int i = 0; ; i++) {
    Task_Sleep( i );
  }
}

void start()
{
  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  Nothing();
}
