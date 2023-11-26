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

#include "ostaskops.h"
#include "processor.h"
#include "raw_memory_manager.h"
#include "mpsafe_dll.h"

// This file contains declarations useful to the OSTask code.

// The following routines must be provided by higher-level code.

// This routine is called once, at startup, in svc32 mode with the
// limited svc stack in workspace.
void __attribute__(( noreturn )) startup();

#define NORET __attribute__(( noinline, noreturn ))

// Run a non-OSTask SWI
// Return 0 to continue the current task, an OSTask to run otherwise
OSTask *execute_swi( svc_registers *regs, int number );

static inline
bool lock_ostask()
{
  return core_claim_lock( &shared.ostask.lock, workspace.core+1 );
}

static inline
void release_ostask()
{
  core_release_lock( &shared.ostask.lock );
}

OSTask *queue_running_OSTask( svc_registers *regs,
                              uint32_t queue_handle,
                              uint32_t SWI );

void save_task_state( svc_registers *regs );

void interrupting_privileged_code( OSTask *task );

typedef struct OSTaskSlot OSTaskSlot;
typedef struct OSTask OSTask;

static inline uint32_t ostask_handle( OSTask *task )
{
  if (task == 0) return 0;
  return 0x4b534154 ^ (uint32_t) task;
}

static inline OSTask *ostask_from_handle( uint32_t h )
{
  if (h == 0) return 0;
  return (OSTask *) (0x4b534154 ^ h);
}

static inline OSQueue *queue_from_handle( uint32_t handle )
{
  if (handle == 0) return 0;

  // TODO check that it's in system heap
  // 0x55455551 = "QUEU" - important that it's an odd number
  // so handles can be distinguished from function pointers.
  return (OSQueue *) (handle ^ 0x55455551);
}

static inline uint32_t queue_handle( OSQueue *queue )
{
  uint32_t handle;
  if (queue == 0)
    handle = 0;
  else
    handle = (0x55455551 ^ (uint32_t) queue);
  return handle;
}

typedef struct {
  uint32_t page_base;   // Pages
  uint32_t pages;       // Pages
  uint32_t va_page:20;  // Start page
  bool     device:1;
  bool     read_only:1;
  uint32_t res:10;
} app_memory_block;

struct OSTaskSlot {
  uint32_t mmu_map;
  uint32_t number_of_tasks;

  // See memory.c
  app_memory_block app_mem[30];
  app_memory_block pipe_mem[30];

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

  union {
    OSTask *controller;
    struct __attribute__(( packed )) {
      uint32_t offset:10; // Big enough for kernel SWIs
      uint32_t core:8;
      uint32_t res:14;
    } swi;
    struct __attribute__(( packed )) {
      uint32_t swi_offset:6;
      uint32_t core:8;
      uint32_t res:16;
      uint32_t match_swi:1;
      uint32_t match_core:1;
    } handler;
  };

  OSTask *next;
  OSTask *prev;
};

MPSAFE_DLL_TYPE( OSTask );

// Application memory management (memory.c)
uint32_t map_device_pages( uint32_t va,
                           uint32_t page_base,
                           uint32_t pages );
uint32_t app_memory_top( uint32_t top );
void map_slot( OSTaskSlot *new );

#include "heap.h"

// Return a 4-byte aligned pointer to an area of at least
// size bytes of privileged writable memory. Or NULL.
// Will not be called until the OSTask subsystem has called startup.
static inline void *system_heap_allocate( uint32_t size )
{
  extern uint8_t system_heap_base;
  return heap_allocate( &system_heap_base, size );
}

// Ditto, except usr accessible and executable memory
static inline void *shared_heap_allocate( uint32_t size )
{
  extern uint8_t shared_heap_base;
  return heap_allocate( &shared_heap_base, size );
}

// Free memory allocated using one of the above
static inline void system_heap_free( void *block )
{
  extern uint8_t system_heap_base;
  heap_free( &system_heap_base, block );
}

static inline void shared_heap_free( void *block )
{
  extern uint8_t shared_heap_base;
  heap_free( &shared_heap_base, block );
}

static inline OSPipe *pipe_from_handle( uint32_t handle )
{
  if (handle == 0) return 0;
  return (OSPipe *) (0x45504950 ^ handle);
}

static inline uint32_t pipe_handle( OSPipe *pipe )
{
  if (pipe == 0) return 0;
  return 0x45504950 ^ (uint32_t) pipe;
}

extern void setup_pipe_pool();

OSTask *PipeCreate( svc_registers *regs );
OSTask *PipeWaitForSpace( svc_registers *regs, OSPipe *pipe );
OSTask *PipeSpaceFilled( svc_registers *regs, OSPipe *pipe );
OSTask *PipeSetSender( svc_registers *regs, OSPipe *pipe );
OSTask *PipeUnreadData( svc_registers *regs, OSPipe *pipe );
OSTask *PipeNoMoreData( svc_registers *regs, OSPipe *pipe );
OSTask *PipeWaitForData( svc_registers *regs, OSPipe *pipe );
OSTask *PipeDataConsumed( svc_registers *regs, OSPipe *pipe );
OSTask *PipeSetReceiver( svc_registers *regs, OSPipe *pipe );
OSTask *PipeNotListening( svc_registers *regs, OSPipe *pipe );

extern void setup_queue_pool();

OSTask *QueueCreate( svc_registers *regs );
OSTask *QueueWait( svc_registers *regs, OSQueue *queue,
                   bool swi, bool core );
