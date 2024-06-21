/* Copyright 2024 Simon Willcocks
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
  uint32_t lock;
  uint32_t output_pipe;
  uint32_t stack[60];
};

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

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

const char title[] = "TestSysVars";
const char help[] = "TestSysVars\t0.01 (" CREATION_DATE ")";

void __attribute__(( noreturn )) Testing()
{
  for (uint32_t n = 0;; n++) {
    register char const *name asm( "r0" ) = "TestVar";
    register uint32_t *val asm( "r1" ) = &n;
    register uint32_t len asm( "r2" ) = 4;
    register uint32_t context asm( "r3" ) = 0;
    register uint32_t type asm( "r4" ) = 1;
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_SetVarVal)
      , "r" (name)
      , "r" (val)
      , "r" (len)
      , "r" (context)
      , "r" (type)
      : "memory", "lr", "cc" );
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

  Testing();
}
