/* Copyright 2025 Simon Willcocks
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
  uint32_t queue;
  struct {
    uint32_t stack[64];
  } runstack;
};

#define MODULE_CHUNK "0x8040"

// SWI 8040 - Make a 

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

const char title[] = "Modern";
const char help[] = "Modern\t\t0.01 (" CREATION_DATE ")";

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

// Change the word at `word' to the value `to' if it contained `from'.
// Returns the original content of word (== from, if changed successfully)
static inline
uint32_t change_word_if_equal( uint32_t *word, uint32_t from, uint32_t to )
{
  uint32_t failed = true;
  uint32_t value;

  do {
    asm volatile ( "ldrex %[value], [%[word]]"
                   : [value] "=&r" (value)
                   : [word] "r" (word) );

    if (value == from) {
      // Assembler note:
      // The failed and word registers are not allowed to be the same, so
      // pretend to gcc that the word may be written as well as read.

      asm volatile ( "strex %[failed], %[value], [%[word]]"
                     : [failed] "=&r" (failed)
                     , [word] "+r" (word)
                     : [value] "r" (to) );

      ensure_changes_observable();
    }
    else {
      asm ( "clrex" );
      break;
    }
  } while (failed);

  return value;
}

__attribute__(( noreturn, naked ))
void swi_handler( uint32_t handle, workspace *ws )
{
  for (;;) {
    queued_task client = Task_QueueWait( ws->queue );
    switch (client.swi) {
    default:
      {
      register char c asm( "r0" ) = 'W';
      asm ( "svc %[swi]" : : [swi] "i" (OS_WriteC), "r" (c) );
      }
    }
    Task_ReleaseTask( client.task_handle, 0 );
  }
  return true;
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  if (*private == 0) {
    int size = sizeof( workspace );
    *private = rma_claim( size );
    memset( *private, 0, size );
  }
  else {
    asm ( "bkpt 1" );
  }

  workspace *ws = *private;

  ws->queue = Task_QueueCreate();
  swi_handlers handlers = { };
  handlers.action[0].queue = ws->queue;
  handlers.action[1].queue = ws->queue;

  Task_RegisterSWIHandlers( &handlers );

  Task_CreateTask1( swi_handler, aligned_stack( &ws->runstack + 1 ), ws );
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

