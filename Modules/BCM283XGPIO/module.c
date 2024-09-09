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

#include "common.h"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#define MODULE_CHUNK "0x400"

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

const char title[] = "BCM283XGPIO";
const char help[] = "RasPi graphics\t0.01";

void spawn_gpio_manager( workspace *ws )
{
  register void *start asm( "r0" ) = adr( gpio_task );
  register uint32_t sp asm( "r1" ) = 0;
  register workspace *r1 asm( "r2" ) = ws;
  register uint32_t new_handle asm( "r0" );
  asm volatile ( // volatile because we ignore output
        "svc %[swi_spawn]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (new_handle)
    , "=r" (sp) // corrupted
    : [swi_spawn] "i" (OSTask_Spawn)
    , [swi_release] "i" (OSTask_ReleaseTask)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void __attribute__(( noinline )) c_init( workspace **private )
{
  if (*private == 0) {
    *private = rma_claim( sizeof( workspace ) );
  }
  else {
    asm ( "udf 1" );
  }

  workspace *ws = *private;
  ws->queue = Task_QueueCreate();

  swi_handlers handlers = { };
  handlers.action[0].queue = ws->queue;
  Task_RegisterSWIHandlers( &handlers );

  spawn_gpio_manager( ws );
}

void __attribute__(( naked )) init()
{
  struct workspace **private;

  // Move r12 into argument register
  asm volatile (
          "push { lr }"
      "\n  mov %[private_word], r12" : [private_word] "=r" (private) );

  c_init( private );
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


