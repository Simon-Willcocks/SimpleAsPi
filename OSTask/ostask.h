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

#include "ostaskops.h"
#include "processor.h"
#include "raw_memory_manager.h"

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

typedef struct OSTaskSlot OSTaskSlot;
typedef struct OSTask OSTask;

static inline uint32_t ostask_handle( OSTask *task )
{
  return 0x4b534154 ^ (uint32_t) task;
}

static inline OSTask *ostask_from_handle( uint32_t h )
{
  return (OSTask *) (0x4b534154 ^ h);
}

typedef struct {
  uint32_t page_base;   // Pages
  uint32_t pages;       // Pages
  uint32_t va_page:20;  // Start page
  bool     device:1;
  uint32_t res:11;
} app_memory_block;

struct OSTaskSlot {
  uint32_t mmu_map;
  uint32_t number_of_tasks;
  OSTaskSlot_extras extras;

  // See memory.c
  app_memory_block app_mem[30];

  // List is only used for free pool, ATM.
  OSTaskSlot *next;
  OSTaskSlot *prev;
};

struct __attribute__(( packed, aligned( 4 ) )) OSTask {
  svc_registers regs;
  uint32_t banked_sp_usr; // Only stored when leaving usr or sys mode
  uint32_t banked_lr_usr; // Only stored when leaving usr or sys mode
  int32_t resumes; // Signed: -1 => blocked
  OSTaskSlot *slot;

  OSTask_extras extras;

  OSTask *next;
  OSTask *prev;
};

// Application memory management (memory.c)
uint32_t map_device_pages( uint32_t va,
                           uint32_t page_base,
                           uint32_t pages );
uint32_t app_memory_top( uint32_t top );
void initialise_app_virtual_memory_area();
void clear_app_virtual_memory_area();
void changing_slot( OSTaskSlot *old, OSTaskSlot *new );
