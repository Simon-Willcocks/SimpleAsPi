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

// The OSTask code doesn't care about the content of the extras, but
// will allocate space for them in OSTask and OSTaskSlot structures.

#include "CK_types.h"
#include "processor.h"
#include "raw_memory_manager.h"

// This include file must define OSTaskSlot_extras and OSTask_extras
#include "ostask_extras.h"

enum {
  OSTask_Yield = 0x300, OSTask_Sleep
} OSTaskSWI;

// These routines return the higher level data
OSTaskSlot_extras *OSTaskSlot_extras_now();
OSTask_extras *OSTask_extras_now();

// The following routines must be provided by higher-level code.

// This routine is called once, at startup, in svc32 mode with the
// limited svc stack in workspace.
void __attribute__(( noreturn )) startup();

// Probable uses will include floating point state, etc.
// Also, hopefully, claiming and releasing the legacy SVC
// stack, so that it can eventually be done away with!
void OSTask_switching_out( uint32_t handle, OSTask_extras *task );
void OSTask_switching_in( uint32_t handle, OSTask_extras *task );

void OSTask_dropping_to_usr32( uint32_t handle, OSTask_extras *task );

#define NORET __attribute__(( noinline, noreturn ))

// Provided by higher layers, may use the utility functions below.
void NORET execute_swi( svc_registers *regs, int number );

void save_task_state( svc_registers *regs );
void NORET resume_task( uint32_t blocked ); // handle of task not currently runnable or running