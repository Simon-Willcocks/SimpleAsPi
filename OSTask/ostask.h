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

// The following two routines must be provided by higher-level code:

// This routine is called once, at startup, in svc32 mode with the
// limited svc stack in workspace.
void __attribute__(( noreturn )) startup();

// Run the SWI (by calling ostask_svc for OSTask SWIs), finishing by
// calling return_to_swi_caller.
void execute_swi( svc_registers *regs, int number );

// The provided execute_swi routine must call this routine for all
// SWIs in the range OSTask_Yield ... OSTask_Yield + 63
OSTask *ostask_svc( svc_registers *regs, int number );

static inline
void __attribute__(( noreturn )) return_to_swi_caller( 
                        OSTask *task,
                        svc_registers *regs,
                        void *svc_sp );

#define assert( x ) do { if (!(x)) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); } while (false)

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

void save_task_state( svc_registers const *regs );

void interrupting_privileged_code( OSTask *task );

typedef struct OSTaskSlot OSTaskSlot;
typedef struct OSTask OSTask;

#ifndef TASK_HANDLE_MAGIC
#define TASK_HANDLE_MAGIC 0x4b534154
#endif

static inline uint32_t ostask_handle( OSTask *task )
{
  if (task == 0) return 0;
  return TASK_HANDLE_MAGIC ^ (uint32_t) task;
}

static inline OSTask *ostask_from_handle( uint32_t h )
{
  if (h == 0) return 0;
  return (OSTask *) (TASK_HANDLE_MAGIC ^ h);
}

#ifndef QUEUE_HANDLE_MAGIC
#define QUEUE_HANDLE_MAGIC 0x55455551
#endif

#if 0 == (QUEUE_HANDLE_MAGIC & 1)
#error "QUEUE_HANDLE_MAGIC must be odd!"
#endif

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

  char const *command;

  // See memory.c
  app_memory_block app_mem[30];
  app_memory_block pipe_mem[100];

  // List is only used for free pool, ATM.
  OSTaskSlot *next;
  OSTaskSlot *prev;
};

#ifndef MAX_CONTROLLERS
// above 5 needs memmove
#define MAX_CONTROLLERS 31
#endif

struct __attribute__(( packed, aligned( 4 ) )) OSTask {
  // Order of the next 4 items fixed for assembler code
  svc_registers regs;
  uint32_t banked_sp_usr; // Only stored when leaving usr or sys mode
  uint32_t banked_lr_usr; // and only if the core runs another task
  OSTaskSlot *slot;     // Current slot.

  OSTaskSlot *home;     // Task's original slot, zero while not running code
                        // for another task.

  union {
    struct __attribute__(( packed )) {
      uint32_t offset:24; // Big enough for all SWIs
      uint32_t core:8;    // Ever used?
    } swi;
    struct __attribute__(( packed )) {
      uint32_t swi_offset:6;
      uint32_t core:8;
      uint32_t res:16;
      uint32_t match_swi:1;
      uint32_t match_core:1;
    } handler;
  };

  // A controlled Task is either waiting to be told what to do, or
  // running whatever code the (latest) controller wants it to run.
  // A controller task can pass a task on to another, either explicitly,
  // or by queuing it for another task to retrieve.
  OSTask *controller[MAX_CONTROLLERS];
  union {
    void *saved;
    uint32_t running:1;
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
void map_first_slot();
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

static inline void shared_heap_free( void const *block )
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

void setup_pipe_pool();

OSTask *TaskOpLogString( svc_registers *regs );
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
OSTask *TaskOpGetLogPipe( svc_registers *regs );

void create_log_pipe();
void LogString( char const *string, uint32_t length );

void setup_queue_pool();

OSTask *QueueCreate( svc_registers *regs );
OSTask *QueueWait( svc_registers *regs, OSQueue *queue,
                   bool swi, bool core );

OSTask *TaskOpLockClaim( svc_registers *regs );
OSTask *TaskOpLockRelease( svc_registers *regs );

void sanity_check();

void sleeping_tasks_add( OSTask *tired );
void sleeping_tasks_tick();

static inline bool push_controller( OSTask *task, OSTask *controller )
{
#ifdef DEBUG__FOLLOW_CONTROLLERS
  {
  Task_LogString( "New controller for ", 19 );
  Task_LogHexP( task );
  Task_LogString( " is ", 4 );
  Task_LogHexP( controller );
  Task_LogString( "... ", 4 );
  int i = 0;
  while (task->controller[i] != 0 && i < MAX_CONTROLLERS) {
    Task_LogHexP( task->controller[i] );
    Task_LogString( " ", 1 );
    i++;
  }
  Task_LogNewLine();
  }
#endif
  if (task->controller[MAX_CONTROLLERS-1] != 0) {
#ifdef DEBUG__FOLLOW_CONTROLLERS
    Task_LogString( "Oops!\n", 6 );
    PANIC;
#endif
    return false;
  }

  int i = 0;
  while (task->controller[i] != 0) i++;
  while (i > 1) {
    task->controller[i] = task->controller[i-1];
    i++;
  }
  task->controller[0] = controller;

#ifdef DEBUG__FOLLOW_CONTROLLERS
  {
  Task_LogString( "Now controllers for ", 20 );
  Task_LogHexP( task );
  Task_LogString( " are ", 5 );
  {
  int i = 0;
  while (task->controller[i] != 0 && i < MAX_CONTROLLERS) {
    Task_LogHexP( task->controller[i] );
    Task_LogString( " ", 1 );
    i++;
  }
  }
  Task_LogNewLine();
  }
#endif

  return true;
}

static inline bool pop_controller( OSTask *task )
{
#ifdef DEBUG__FOLLOW_CONTROLLERS
  Task_LogString( "Removing controller for ", 25 );
  Task_LogHexP( task );
  int i = 0;
  while (task->controller[i] != 0 && i < MAX_CONTROLLERS) {
    Task_LogString( " ", 1 );
    Task_LogHexP( task->controller[i] );
    i++;
  }
  Task_LogNewLine();
#endif
  if (task->controller[0] == 0) return false;

  for (int i = 0; i < MAX_CONTROLLERS-1; i++) {
    task->controller[i] = task->controller[i+1];
    if (task->controller[i] == 0) break;
  }
  // In case the stack was full:
  task->controller[MAX_CONTROLLERS-1] = 0;

#ifdef DEBUG__FOLLOW_CONTROLLERS
  {
  Task_LogString( "Now controllers for ", 20 );
  Task_LogHexP( task );
  Task_LogString( " are ", 5 );
  {
  int i = 0;
  while (task->controller[i] != 0 && i < MAX_CONTROLLERS) {
    Task_LogHexP( task->controller[i] );
    Task_LogString( " ", 1 );
    i++;
  }
  }
  Task_LogNewLine();
  }
#endif
  return true;
}

static inline OSTask *current_controller( OSTask *task )
{
#ifdef DEBUG__FOLLOW_CONTROLLERS
  Task_LogString( "Current controller for ", 23 );
  Task_LogHexP( task );
  Task_LogString( " ", 1 );
  Task_LogHexP( task->controller[0] );
  Task_LogNewLine();
#endif
  return task->controller[0];
}

static inline void change_current_controller( OSTask *task, OSTask *new )
{
#ifdef DEBUG__FOLLOW_CONTROLLERS
  Task_LogString( "Change controller for ", 22 );
  Task_LogHexP( task );
  Task_LogString( " from ", 6 );
  Task_LogHexP( task->controller[0] );
  Task_LogString( " to ", 4 );
  Task_LogHexP( new );
  Task_LogNewLine();
#endif
  if (0 == task->controller[0]) PANIC;
  task->controller[0] = new;
}

static inline
bool needs_usr_stack( svc_registers const *regs )
{
#ifdef ALLOW_SYS_MODE
  // If the kernel absolutely, positively has to have a protected mode
  // that uses the user stack.
  // Legacy uses it for centisecond ticks, calls to TickerV.
  return (0 == (regs->spsr & 15)) || (15 == (regs->spsr & 15));
#else
  return 0 == (regs->spsr & 15);
#endif
}

// This calls Task_EndTask from usr32 mode
void unexpected_task_return();

static inline
void put_usr_registers( OSTask *task )
{
  asm ( "msr sp_usr, %[sp]"
    "\n  msr lr_usr, %[lr]"
    :
    : [sp] "r" (task->banked_sp_usr)
    , [lr] "r" (task->banked_lr_usr) );
}

static inline
void get_usr_registers( OSTask *task )
{
  asm ( "mrs %[sp], sp_usr"
    "\n  mrs %[lr], lr_usr"
      : [sp] "=r" (task->banked_sp_usr)
      , [lr] "=r" (task->banked_lr_usr) );
}

static inline
OSTask *stop_running_task( svc_registers const *regs )
{
  OSTask *running = workspace.ostask.running;
  assert( running != workspace.ostask.idle );
  OSTask *next = running->next;
  assert( !next->running );
  assert( running->running );
  save_task_state( regs );
  assert( !running->running );
  workspace.ostask.running = next;
  assert( running != workspace.ostask.running );
  dll_detach_OSTask( running );
  assert( next == workspace.ostask.running );
  assert( workspace.ostask.running != running );
  return next;
}

static inline
void __attribute__(( noreturn )) return_to_swi_caller( 
                        OSTask *task,
                        svc_registers *regs,
                        void *svc_sp )
{
  if (task != 0) {
    assert( task->saved );

    assert( task == workspace.ostask.running );
    if (needs_usr_stack( regs )) {
      put_usr_registers( task );
    }

    assert( !task->running );
    task->running = 1;
  }
  else {
    assert( workspace.ostask.running->running );
  }

  assert( task == 0 || task == workspace.ostask.running );

  if (task != 0) map_slot( task->slot );

  // Resume after the SWI
  register svc_registers *lr asm ( "lr" ) = regs;
  asm (
          "mov sp, %[sp]"
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : [sp] "r" (svc_sp)
      , "r" (lr) );

  __builtin_unreachable();
}
