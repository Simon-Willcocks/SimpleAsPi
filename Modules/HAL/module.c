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

typedef struct workspace workspace;

struct workspace {
  uint32_t lock;
};

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

#include "ostaskops.h"

//NO_start;
NO_init;
NO_finalise;
NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "HAL";
const char help[] = "RasPi3 HAL\t0.01 (" CREATION_DATE ")";

void __attribute__(( noreturn )) boot( char const *cmd, workspace *ws )
{
  char command[64];
  char *mod;
  // -fPIE does insane stuff with strings, copying the rodata string
  // to the stack so that I can copy it onto the stack from the wrong
  // address...
  // I think the offset tables are supposed to be fixed up by the standard
  // library (which isn't going to work in ROM modules, is it?).
  // Compile ROM modules as fixed location, but check there's no .data segment
  // TODO
  asm ( "mov %[m], #0"
    "\n0:"
    "\n  ldrb lr, [%[s], %[m]]"
    "\n  cmp lr, #0"
    "\n  strb lr, [%[d], %[m]]"
    "\n  addne %[m], %[m], #1"
    "\n  bne 0b"
    "\n  add %[m], %[d], %[m]"
    : [m] "=&r" (mod)
    : [s] "r" ("System:Modules.")
    , [d] "r" (command)
    : "lr" );

  // INITIAL_MODULES provided by build script.
  // Must be a single string consiting of nul-terminated module
  // names, e.g. "QA7\0BCM_GPIO\0". (That final null is important,
  // it ensures the string is double-nul-terminated.)
  char const *s = INITIAL_MODULES;

  while (*s != '\0') {
    char *p = mod;
    do {
      *p++ = *s++;
    } while (*s != '\0');
    *p++ = '\n'; // Terminator
    s++;

    Task_LogString( command, p - command );
    register uint32_t load asm ( "r0" ) = 1; // RMLoad
    register char const *module asm ( "r1" ) = command;
    register error_block *error asm ( "r0" );

    asm ( "svc %[swi]"
      "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OS_Module), "r" (load), "r" (module)
      : "cc", "memory" );
    // "memory" is required because the SWI accesses memory
    // (the module name). Without it, the final *p = '\0'; may
    // be delayed until after the SWI is called.

    if (error != 0) {
      asm ( "udf 7" );
    }

    // In case the initialisation kicked off some tasks, let them run!
    // Legacy modules might have set callbacks, they'll have been run
    // by the time we get here. (Modules are not permitted to call
    // Yield in their initialisation code, svc mode.)
    Task_Yield();
  }

  char const entering[] = "Entering default language: " DEFAULT_LANGUAGE "\n";
  Task_LogString( entering, sizeof( entering ) - 1 );
  register uint32_t enter asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules."DEFAULT_LANGUAGE;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
    "\n  movvc r0, #0"
    : "=r" (error)
    : [swi] "i" (OS_Module), "r" (enter), "r" (module)
    : "cc", "memory" );

  for (;;) { asm ( "udf 8" ); Task_Sleep( 1000000 ); }

  __builtin_unreachable();
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

void __attribute__(( naked )) start( char const *command )
{
  register workspace *ws asm ( "r12" );

  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  boot( command, ws );

  __builtin_unreachable();
}

