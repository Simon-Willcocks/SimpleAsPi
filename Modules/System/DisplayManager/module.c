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

/*
  SWIs

    Register driver
      IN
        name
        max displays
        queue / pipe
      OUT
        id

    Register display (screen)
      IN
        resolution
        control queue/pipe
*/


typedef struct workspace workspace;

// FIXME: allocate a chunk!
#define MODULE_CHUNK "0x2000"

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

const char title[] = "DisplayManager";
const char help[] = "DisplayManager\t0.01 (" CREATION_DATE ")";

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

#include "bcm_gpu.h"

struct workspace {
  outstanding_request request[MAX_REQUESTS];
  uint32_t queue;
  uint32_t response_task;
  struct {
    uint32_t stack[62];
  } response_manager_stack;
  struct {
    uint32_t stack[64];
  } mailbox_manager_stack;
};


void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  *private = rma_claim( sizeof( workspace ) );
  workspace *ws = *private;

  memset( ws, 0, sizeof( workspace ) );

  ws->queue = Task_QueueCreate();

  swi_handlers handlers = { };
  for (int i = 0; i < 16; i++) {
    handlers.action[i].queue = ws->queue;
  }

  Task_RegisterSWIHandlers( &handlers );

  register void *start asm( "r0" ) = response_manager;
  register uint32_t sp asm( "r1" ) = aligned_stack( &ws->response_manager_stack + 1 );
  register workspace *r1 asm( "r2" ) = ws;
  register uint32_t handle asm( "r0" );
  asm volatile (
        "svc %[swi_spawn]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (sp)
    , "=r" (handle)
    : [swi_spawn] "i" (OSTask_Spawn)
    , [swi_release] "i" (OSTask_ReleaseTask)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
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

