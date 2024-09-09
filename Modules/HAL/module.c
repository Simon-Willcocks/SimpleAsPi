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
  static char const base[] = "System:Modules.";
  char command[128];
  char *mod = command;
  char const *b = base;
  while (*b != 0) {
    *mod++ = *b++;
  }

  // INITIAL_MODULES provided by build script.
  // Must be a single string consiting of nul-terminated module
  // names, e.g. "QA7\0BCM_GPIO\0". (That final null is important,
  // it ensures the string is double-nul-terminated.)
  char const *s = INITIAL_MODULES;

  while (*s != '\0') {
    char *p = mod;
    do {
      *p++ = *s++;
    } while (*s >= ' ');
    *p++ = '\n'; // Terminator
    s++;

    Task_LogString( command, p - command );
    Task_Yield();

    register uint32_t load asm ( "r0" ) = 1; // RMLoad
    register char const *module asm ( "r1" ) = command;
    register error_block *error asm ( "r0" );

    asm ( "svc %[swi]"
      "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OS_Module), "r" (load), "r" (module)
      : "cc", "memory" );
    // "memory" is required because the SWI accesses memory
    // (the module name). Without it, the final *p = '\n'; may
    // be delayed until after the SWI is called.

    if (error != 0) {
      asm ( "udf 7" );
    }

    // In case the initialisation kicked off some tasks, let them run!
    // Legacy modules might have set callbacks, they'll have been run
    // by the time we get here. (Modules are not permitted to call
    // Yield in their svc mode initialisation code.)

    // Task_Yield(); Moved to after the LogString and after the loop
  }

  Task_Yield();

  static char const entering[] = "Entering default language: " DEFAULT_LANGUAGE " in ";
  Task_LogString( entering, sizeof( entering ) - 1 );

if (0) Task_Sleep( 10 );
if (0) {
static char const starting[] = "About to start...";
register char const *s asm( "r0" ) = starting;
asm volatile ( "svc %[swi]" : "=r" (s) : [swi] "i" (OS_Write0), "r" (s) );
}
if (0) {
static char const cmd[] = "Set Wimp$Font Trinity.Medium";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}
if (0) {
static char const cmd[] = "Echo <Wimp$Font>";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}


if (1) {
static char const varname[] = "Wimp$Font";
static char const value[] = "Trinity.Medium";
register char const *var asm( "r0" ) = varname;
register char const *val asm( "r1" ) = value;
register uint32_t len asm( "r2" ) = sizeof( value ) - 1;
register uint32_t ctx asm( "r3" ) = 0;
// register uint32_t type asm( "r4" ) = 0; // String to GSTrans on set
register uint32_t type asm( "r4" ) = 0; // Literal String
asm ( "svc %[swi]" : 
    : [swi] "i" (OS_SetVarVal)
    , "r" (var)
    , "r" (val)
    , "r" (len)
    , "r" (ctx)
    , "r" (type) );
{
static const char str[] = "Set ";
register char const *s asm( "r0" ) = str;
asm ( "svc %[swi]" : : [swi] "i" (OS_Write0), "r" (s) );
}
{
register char const *s asm( "r0" ) = varname;
asm ( "svc %[swi]" : : [swi] "i" (OS_Write0), "r" (s) );
}
{
register char const *s asm( "r0" ) = " to ";
asm ( "svc %[swi]" : : [swi] "i" (OS_Write0), "r" (s) );
}
{
register char const *s asm( "r0" ) = value;
asm ( "svc %[swi]" : : [swi] "i" (OS_Write0), "r" (s) );
}
}


if (0) {
static char const cmd[] = "Echo Hello world";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}

if (0) {
static char const cmd[] = "Show";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}

    if (0) Task_Sleep( 10 );
if (0) {
char const cmd[] = "Status";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}

    if (0) Task_Sleep( 10 );
{
char const cmd[] = "Modules";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}

  for (int i = 2; i > 0; i--) {
    Task_LogSmallNumber( i );
    Task_LogString( " ", 1 );
    Task_Sleep( 10 );
  }
{
char const cmd[] = "Desktop";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}
    Task_LogString( "Desktop returned!\n", 17 );

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

