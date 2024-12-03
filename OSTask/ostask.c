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

#include "ostask.h"

// OK, to start with, temporarily, 1MiB sections
extern OSTask OSTask_free_pool[];
extern OSTaskSlot OSTaskSlot_free_pool[];

MPSAFE_DLL_TYPE( OSTaskSlot );

#ifdef DEBUG__EMERGENCY_UART_ACCESS
#include "bcm_uart.h"

static inline void send_char( char c )
{
  UART volatile * const uart = (void*) 0xfffff000;

  while (UART_TxEmpty != (uart->flags & UART_TxEmpty)) {
    for (int i = 0; i < 10000; i++) asm ( "" );
    while (UART_TxEmpty != (uart->flags & UART_TxEmpty)) {
      for (int i = 0; i < 1000; i++) asm ( "" );
    }
  }

  uart->data = c;
}

void __attribute__(( noinline )) send_number( uint32_t n, char c )
{
  char const hex[] = "0123456789abcdef";

  for (int i = 28; i >= 0; i-=4)
    send_char( hex[0xf & (n >> i)] );

  if (c != '\0') send_char( c );
}
#else
static inline void send_number( uint32_t n, char c ) {}
static inline void send_char( char c ) {}
#endif

void setup_pools()
{
  memory_mapping tasks = {
    .base_page = claim_contiguous_memory( 0x100 ), // 1 MiB
    .pages = 0x100,
    .vap = &OSTask_free_pool,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (tasks.base_page == 0xffffffff) PANIC;
  map_memory( &tasks );
#ifdef DEBUG__USER_TASKS_ACCESS
  {
    tasks.type = CK_MemoryRW;
    tasks.va = DEBUG__USER_TASKS_ACCESS;
    tasks.usr32_access = 1;
    map_memory( &tasks );
  }
#endif

  for (int i = 0; i < 100; i++) { // FIXME How many in a MiB?
    OSTask *t = &OSTask_free_pool[i];
    memset( t, 0, sizeof( OSTask ) );
    dll_new_OSTask( t );
    dll_attach_OSTask( t, &shared.ostask.task_pool );
    shared.ostask.task_pool = shared.ostask.task_pool->next;
  }

  memory_mapping slots = {
    .base_page = claim_contiguous_memory( 0x100 ), // 1 MiB
    .pages = 0x100,
    .vap = &OSTaskSlot_free_pool,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (slots.base_page == 0xffffffff) PANIC;
  map_memory( &slots );
  for (int i = 0; i < 100; i++) { // FIXME How many in a MiB?
    OSTaskSlot *s = &OSTaskSlot_free_pool[i];
    memset( s, 0, sizeof( OSTaskSlot ) );
    dll_new_OSTaskSlot( s );
    s->mmu_map = i; // FIXME: Will run out of values at 65536
    dll_attach_OSTaskSlot( s, &shared.ostask.slot_pool );
    shared.ostask.slot_pool = shared.ostask.slot_pool->next;
  }

  setup_pipe_pool();
  setup_queue_pool();
}

static void setup_processor_vectors();

void __attribute__(( naked, noreturn )) idle_task();

void __attribute__(( noreturn )) boot_with_stack( uint32_t core )
{
#ifdef DEBUG__BREAKME_STARTUP_CORE_NON_ZERO
#ifdef DEBUG__SINGLE_CORE
#error "Not the smartest combination of options!"
#endif
  OSTaskSlot *volatile *p = &shared.ostask.first;
  while (*p == 0 && core == 0) {}
#endif

  forget_boot_low_memory_mapping();

  bool reclaimed = core_claim_lock( &shared.ostask.lock, core + 1 );
  if (reclaimed) PANIC;

  bool first = (shared.ostask.first == 0);

  workspace.core = core;

  if (first)
  {
    shared.ostask.number_of_cores = number_of_cores();

    extern uint8_t top_of_boot_RAM;
    extern uint8_t top_of_minimum_RAM;

    // Some RAM to be going on with...
    free_contiguous_memory( ((uint32_t) &top_of_boot_RAM) >> 12,
                            (&top_of_minimum_RAM - &top_of_boot_RAM) >> 12 );

    setup_pools();

    shared.ostask.first = mpsafe_detach_OSTaskSlot_at_head( &shared.ostask.slot_pool );
  }

  setup_processor_vectors();

  release_ostask();

  create_log_pipe();

  // Make this running code into an OSTask
  workspace.ostask.running = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );

  workspace.ostask.running->slot = shared.ostask.first;
  map_first_slot();

  mmu_establish_resources();

#ifdef DEBUG__ENABLE_EVENT_STREAM
  // Setup event stream to see if I've missed something...
  // This will re-start cores waiting for events if the software "forgets"
  // to signal one.
  asm ( "mcr p15, 0, %[start], c14, c1, 0" : : [start] "r" (0x44) );
#endif

  if (first) {
    // Create a separate idle OSTask
    workspace.ostask.idle = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );
    workspace.ostask.idle->regs.lr = (uint32_t) idle_task;

    // usr32, interrupts enabled
    workspace.ostask.idle->regs.spsr = 0x10;
    workspace.ostask.idle->slot = shared.ostask.first;

    dll_attach_OSTask( workspace.ostask.idle, &workspace.ostask.running );
    workspace.ostask.running = workspace.ostask.running->next;

#ifdef DEBUG__FOLLOW_OS_TASKS
Task_LogString( "Initial idle task ", 18 );
Task_LogHex( (uint32_t) workspace.ostask.idle );
Task_LogNewLine();
#endif

    startup();
  }
  else {
    // Become the idle OSTask
    workspace.ostask.idle = workspace.ostask.running;
    workspace.ostask.idle->slot = shared.ostask.first;

    // Before we start scheduling tasks from other cores, ensure the shared
    // memory areas that memory_fault_handlers might use are mapped into this
    // core's translation tables. (The first core in already has them.)

    // This is not the prettiest approach, but I think it should trigger
    // a data abort.
    asm volatile ( "ldr r0, [%[mem]]" : : [mem] "r" (&OSTaskSlot_free_pool) );

    asm ( "mov sp, %[reset_sp]"
      "\n  cpsie aif, #0x10"
      :
      : [reset_sp] "r" ((&workspace.svc_stack)+1) );

    idle_task();
  }

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) idle_task()
{
  asm ( "mov sp, #-1" );
  asm ( "mov lr, #-2" );

  // Yield will usually check for other OSTasks that want to be run
  // and schedule one or return immediately. When called by the core's
  // idle OSTask, it will not return until an OSTask has become runnable
  // (and that one yields or blocks).
  // NOTE!!! If this task is changed to do anything else, IdleTaskYield
  // will need changing to save its state.
  // Supplemental: Do not change this task!
  asm ( "svc %[yield]"
    "\n  b idle_task"
    :
    : [yield] "i" (OSTask_Yield) );
}

void __attribute__(( naked )) reset_handler()
{
  PANIC;
}

void __attribute__(( naked )) undefined_instruction_handler()
{
  // Return to the next instruction. It's useful as a debugging tool
  // to insert e.g. asm ( "udf 1" ) to see the registers at that point.
  // Note: udf #0 is sometimes inserted by the compiler, if it knows
  // an instruction will fail, like if (p == 0) *p = 12;
  asm volatile (
        "srsdb sp!, #0x1b // Store return address and SPSR (UND mode)"
    "\n  rfeia sp! // Restore execution and SPSR"
      );
}

// This routine must be called before moving a task away from this
// core's running task.
// Exception: the entry to irq_handler performs the same task as this.

// TODO: Storing floating point registers as needed.
void __attribute__(( noinline )) save_task_state( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;

  running->regs = *regs;
#ifdef DEBUG__UDF_ON_SAVE_TASK_STATE
  asm ( "udf 3" );
#endif
  if (running->regs.lr == 0) PANIC;

  if (0 == (regs->spsr & 15)) {
    asm ( "mrs %[sp], sp_usr"
      "\n  mrs %[lr], lr_usr"
        : [sp] "=r" (running->banked_sp_usr)
        , [lr] "=r" (running->banked_lr_usr) );
  }
}

void __attribute__(( naked, optimize( "O4" ) )) unexpected_task_return()
{
  // Running in usr32
  // Optimize attribute ensures no stack use.
  Task_EndTask();
}

DEFINE_ERROR( UnknownSWI, 0x1e6, "Unknown SWI" );
DEFINE_ERROR( UnknownPipeSWI, 0x888, "Unknown Pipe operation" );
DEFINE_ERROR( InvalidPipeHandle, 0x888, "Invalid Pipe handle" );
DEFINE_ERROR( InvalidQueue, 0x888, "Invalid OSTask Queue handle" );
DEFINE_ERROR( UnknownQueueSWI, 0x888, "Unknown Queue operation" );

DEFINE_ERROR( NotATask, 0x666, "Programmer error: Not a task" );
DEFINE_ERROR( NotYourTask, 0x667, "Programmer error: Not your task" );
DEFINE_ERROR( InvalidInitialStack, 0x668, "Tasks must always be started with 8-byte aligned stack" );

static inline
OSTask *TaskOpRunForTask( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *client = ostask_from_handle( regs->r[0] );

#ifdef DEBUG__FOLLOW_TASKS
  { char const text[] = "Task ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHex( (uint32_t) running );
  { char const text[] = " switching to slot of Task ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHex( regs->r[0] );
  { char const text[] = ", slot ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHex( (uint32_t) client->slot );
  { char const text[] = ", from slot ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHex( (uint32_t) running->slot );
  Task_LogNewLine();
#endif

  if (client == 0) {
    PANIC;      // FIXME remove
    return Error_NotATask( regs );
  }

  if (current_controller( client ) != running) {
    PANIC;      // FIXME remove
    return Error_NotYourTask( regs );
  }

  if (running->home != 0) PANIC; // Already running for another task FIXME error

  running->home = running->slot;
  running->slot = client->slot;
  map_slot( running->slot );

  if (running->home == 0) PANIC;

  return 0;
}

static inline
OSTask *TaskOpReleaseTask( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *release = ostask_from_handle( regs->r[0] );
  svc_registers *context = (void*) regs->r[1];

#ifdef DEBUG__FOLLOW_TASKS
    { char const text[] = "Task ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( running ) );
    { char const text[] = " releasing ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( release ) );
    { char const text[] = " context ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( context );
    Task_LogNewLine();
    if (context != 0) {
      for (int i = 0; i < 15; i++) {
        Task_LogHex( ((uint32_t*)context)[i] );
        Task_LogString( " ", 1 );
      }
    }
    Task_LogNewLine();
#endif
  if (release == 0) {
    return Error_NotATask( regs );
  }

  if (current_controller( release ) != running) {
    return Error_NotYourTask( regs );
  }

  // Giving up control over the given task
  pop_controller( release );

  if (context != 0) {
    release->regs = *context;
  }

  bool lock_to_core = 0 != (release->regs.spsr & 0x80);

  // This must be done before the release Task is known to be runnable
  if (lock_to_core) {
#ifdef DEBUG__FOLLOW_TASKS
    { char const text[] = "Task ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( release ) );
    { char const text[] = " ready to run on same core\n";
    Task_LogString( text, sizeof( text )-1 ); }
#endif
    OSTask *next = running->next;
    dll_attach_OSTask( release, &next );
  }
  else {
#ifdef DEBUG__FOLLOW_TASKS
    { char const text[] = "Task ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( release ) );
    { char const text[] = " made runnable\n";
    Task_LogString( text, sizeof( text )-1 ); }
#endif
    // The caller's going to keep running, let one of the other cores
    // pick up the resumed task.
    //   TODO When should FP context be stored? Tasks put into runnable
    //   must not own the FP for the current core.
    mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, release );
  }

  return 0;
}

static inline
OSTask *TaskOpChangeController( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *release = ostask_from_handle( regs->r[0] );
  if (release == 0) {
    return Error_NotATask( regs );
  }
  else {
    if (current_controller( release ) != running) {
      PANIC;
    }
    OSTask *new_controller = ostask_from_handle( regs->r[1] );
    if (new_controller == 0)
      return Error_NotATask( regs );
    change_current_controller( release, new_controller );
  }
  return 0;
}

static inline
OSTask *TaskOpSetController( svc_registers *regs )
{
  PANIC; // No longer needed?
  OSTask *running = workspace.ostask.running;
  OSTask *controller = ostask_from_handle( regs->r[1] );
  svc_registers *swi_regs = (void*) regs->r[0];

  if (controller == 0) PANIC;
  // ??? if (current_controller( controller ) != 0) PANIC;
  if (0 == (regs->spsr & 0x80)) PANIC;

  running->controller[0] = controller;

  save_task_state( swi_regs );
  workspace.ostask.running = running->next;
  dll_detach_OSTask( running );

  // Return 0 to ensure the code resumes after this SWI, even
  // though the original task is detached.
  return 0;
}

static inline
OSTask *TaskOpFinished( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;

#ifdef DEBUG__FOLLOW_TASKS
  { char const text[] = "Task ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHex( (uint32_t) running );
  { char const text[] = " finished running for another Task, slot ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHex( (uint32_t) running->slot );
  Task_LogNewLine();
#endif

  if (running->home == 0) PANIC;

  running->slot = running->home;
  running->home = 0;
  map_slot( running->slot );

  return 0;
}

static inline
OSTask *TaskOpGetRegisters( svc_registers *regs )
{
  OSTask *controlled = ostask_from_handle( regs->r[0] );
  svc_registers *context = (void*) regs->r[1];
  OSTask *running = workspace.ostask.running;

  if (controlled == 0) {
    return Error_NotATask( regs );
  }
  if (current_controller( controlled ) != running) {
    return Error_NotYourTask( regs );
  }

  *context = controlled->regs;

#ifdef DEBUG__FOLLOW_TASKS
    { char const text[] = "Task ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( controlled ) );
    { char const text[] = " registers requested to ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( context );
    Task_LogNewLine();
    if (context != 0) {
      for (int i = 0; i < 15; i++) {
        Task_LogHex( ((uint32_t*)context)[i] );
        Task_LogString( " ", 1 );
      }
    }
    Task_LogNewLine();
#endif
  return 0;
}

static inline
OSTask *TaskOpSetRegisters( svc_registers *regs )
{
  OSTask *controlled = ostask_from_handle( regs->r[0] );
  svc_registers *context = (void*) regs->r[1];
  OSTask *running = workspace.ostask.running;

  if (controlled == 0) {
    return Error_NotATask( regs );
  }
  if (current_controller( controlled ) != running) {
    return Error_NotYourTask( regs );
  }

  controlled->regs = *context;

  return 0;
}

static __attribute__(( noinline )) OSTask *for_core( OSTask *volatile *head, void *p )
{
  OSTask *first = *head;
  OSTask *t = first;
  uint32_t core = (uint32_t) p;

  if (t == 0) return 0;

  do {
    if (t->regs.r[0] == core) {
      if (t == t->next) {
        *head = 0;
      }
      else {
        if (t == first) {
          *head = first->next;
        }
        dll_detach_OSTask( t );
      }
      return t;
    }

    t = t->next;
  } while (t != first);

  return 0;
}

//static inline
__attribute__(( noinline ))
OSTask *find_task_for_this_core()
{
  OSTask *volatile *head = &shared.ostask.moving;

  // Single word reads are atomic
  if (*head == 0) return 0; // 99.999% of the time

  return mpsafe_manipulate_OSTask_list_returning_item( head,
                                                       for_core,
                                                       (void*)workspace.core );
}

// static inline
__attribute__(( noinline ))
OSTask *IdleTaskYield( svc_registers *regs )
{
  // Special case; the idle task never leaves its core.

  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;

  assert( running == workspace.ostask.idle );

  if (0 != (regs->spsr & 0x80)) {
    // Finished all IRQ tasks

    // FIXME: It may be possible to ensure that ResumeTask by the irq task
    // doesn't result in the idle task being scheduled before the irq task
    // resumes...
    // Then we could
    // assert( next == running );
    if (next != running) {
      workspace.ostask.running = next;
      // Not saving state; the only thing the idle task does is yield
      return next;
    }

    if (0 != workspace.ostask.interrupted_tasks) {
      // Without skipping the interrupted task it is possible that running
      // more tasks than the number of cores with lots to do between SWIs
      // will lock out all other tasks.
      workspace.ostask.running = workspace.ostask.interrupted_tasks->next;
      workspace.ostask.interrupted_tasks = 0;

      dll_attach_OSTask( running, &workspace.ostask.running );
    }

    assert( workspace.ostask.running == running );

    // We return to idle task with interrupts enabled, even if there's
    // another active task.
    // That way, it can pick up any task that's been activated by an
    // interrupt, which should take priority over previously running
    // tasks.

    regs->spsr &= ~0x80; // Enable interrupts

    return 0;
  }

  save_task_state( regs );

  // Pull a task from either the queue of tasks for this core,
  // or from the general runnable list.

  OSTask *resume = find_task_for_this_core();

  if (resume != 0) {
#ifdef DEBUG__FOLLOW_TASKS
    Task_LogString( "F ", 2 );
    Task_LogHex( (uint32_t) resume );
    Task_LogString( " ", 1 );
    Task_LogSmallNumber( workspace.core );
    Task_LogNewLine();
#endif
    // If we got a task, it was requesting this core, right?
    assert( resume->regs.r[0] == workspace.core );
  }

  if (resume == 0 && shared.ostask.runnable != 0) {
    // Pull from general runnable list, if anything in it.
    // The above check is mp-safe, because we'll receive an envent
    // notification if anyone adds to the list.
    resume = mpsafe_detach_OSTask_at_head( &shared.ostask.runnable );

#ifdef DEBUG__FOLLOW_TASKS_A_LOT
    if (resume != 0) {
      Task_LogString( "R ", 2 );
      Task_LogHex( ostask_handle( resume ) );
      Task_LogNewLine();
    }
#endif
  }

  if (resume == 0
   && next == workspace.ostask.idle) {
    // Only the idle task is runnable on this core right now.

    asm  ( "udf 99" );
    // Pause, then drop back to idle task (interrupts enabled),
    // after which the runnable list will be checked again.
    // wait_for_event();
  }

  if (resume != 0) {
    // resume has been taken from another list

    dll_attach_OSTask( resume, &workspace.ostask.running );
    // Now resume and idle are in the list (resume at the head)
  }
  else if (running != next) {
    workspace.ostask.running = next;
    resume = next;
  }

  return resume;
}

static inline
OSTask *TaskOpYield( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

#ifdef DEBUG__FOLLOW_TASKS_A_LOT
if (running == workspace.ostask.idle && resume != running) {
Task_LogString( "Y ", 2 );
if (running == workspace.ostask.idle)
  Task_LogString( "I ", 2 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogString( " ", 1 );
Task_LogSmallNumber( workspace.core );
Task_LogNewLine();
}
#endif

  if (running == workspace.ostask.idle) {
    resume = IdleTaskYield( regs );
  }
  else {
    // Put at the end of the general runnable list.
    save_task_state( regs );
    workspace.ostask.running = resume;
    dll_detach_OSTask( running );

#ifdef DEBUG__FOLLOW_TASKS_A_LOT
    Task_LogString( "S ", 2 );
    Task_LogHex( ostask_handle( running ) );
    Task_LogNewLine();
#endif
    mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, running );
  }

  return resume;
}

static inline
OSTask *TaskOpSleep( svc_registers *regs )
{
#ifdef DEBUG__NO_SLEEP
  return TaskOpYield( regs );
#endif
  if (regs->r[0] == 0) return TaskOpYield( regs );

#ifdef DEBUG__FOLLOW_TASKS
Task_LogString( "S ", 2 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogString( " ", 1 );
Task_LogSmallNumber( workspace.core );
Task_LogNewLine();
#endif

  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

  assert( resume != running || running == workspace.ostask.idle );

  save_task_state( regs );
  workspace.ostask.running = resume;
  dll_detach_OSTask( running );

  sleeping_tasks_add( running );

  return resume;
}

static inline
OSTask *TaskOpCreate( svc_registers *regs, bool spawn )
{
  // Check parameters BEFORE allocating resources.
  if ((regs->r[1] & 7) != 0)
    return Error_InvalidInitialStack( regs );

  OSTask *running = workspace.ostask.running;
  OSTask *task = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );

  assert( task->next == task || task->prev == task );
  for (int i = 0; i < sizeof( *task ) / 4; i++) {
    *((uint32_t*) task + i) = 0;
  }
  task->next = task;
  task->prev = task;

  if (spawn) {
    task->slot = mpsafe_detach_OSTaskSlot_at_head( &shared.ostask.slot_pool );
  }
  else {
    task->slot = running->slot;
  }
  task->regs.lr = regs->r[0];
  task->regs.spsr = 0x10;
  task->banked_sp_usr = regs->r[1];
  task->banked_lr_usr = (uint32_t) unexpected_task_return;
  task->regs.r[0] = ostask_handle( task );
  task->regs.r[1] = regs->r[2];
  task->regs.r[2] = regs->r[3];
  task->regs.r[3] = regs->r[4];
  task->regs.r[4] = regs->r[5];

  if (!push_controller( task, running )) PANIC;

#ifdef DEBUG__FOLLOW_OS_TASKS
#ifndef DEBUG__FOLLOW_OS_TASKS_CREATION
#define DEBUG__FOLLOW_OS_TASKS_CREATION
#endif
#endif

#ifdef DEBUG__FOLLOW_OS_TASKS_CREATION
Task_LogString( "Created task ", 13 );
Task_LogHex( task->regs.r[0] );
Task_LogString( ", starting at ", 14 );
Task_LogHex( regs->r[0] );
Task_LogString( ", in slot ", 0 );
Task_LogHex( (uint32_t) task->slot );
Task_LogString( ": ", 2 );
Task_LogHex( task->regs.r[1] );
Task_LogString( ", ", 2 );
Task_LogHex( task->regs.r[2] );
Task_LogString( ", ", 2 );
Task_LogHex( task->regs.r[3] );
Task_LogString( ", ", 2 );
Task_LogHex( task->regs.r[4] );
Task_LogNewLine();
#endif

  // The new task will only start executing when the controller lets it.
  // If it has to run on the same core as the controller, it should be
  // started with interrupts disabled.

  regs->r[0] = ostask_handle( task );

  return 0;
}

static inline
OSTask *TaskOpEndTask( svc_registers *regs )
{
#ifdef DEBUG__FOLLOW_OS_TASKS
Task_LogString( "Ended task ", 0 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogNewLine();
#endif
  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

  assert( resume != running || running == workspace.ostask.idle );

  save_task_state( regs );
  workspace.ostask.running = resume;
  dll_detach_OSTask( running );

  return resume;

// FIXME
  regs->r[0] = 0xffffffff; // Maximum sleep
  regs->lr = unexpected_task_return;
  regs->spsr = 0x10;
  return TaskOpSleep( regs );

  extern uint32_t new_queue();

  // FIXME: Tidy up the task, look for any locks held by it, perhaps?
  // If it's the last one in the slot, free the slot, any pipes it has
  // open, etc. Lots to do!
  uint32_t *queue_handle = &shared.ostask.terminated_tasks_queue;
  while (*queue_handle <= 1) {
    uint32_t old = change_word_if_equal( queue_handle, 0, 1 );
    if (old == 0) {
      *queue_handle = new_queue();
    }
  }

  // This has no purpose other than to sideline the task.
  // There has to be a proper implementation
  return queue_running_OSTask( regs, *queue_handle, 0 );
}

static inline
OSTask *TaskOpWaitForInterrupt( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;

  if (0 == (regs->spsr & 0x80)) PANIC; // Must always have interrupts disabled
  if (workspace.ostask.irq_task != 0) PANIC; // TODO Sin bin, both of these

  save_task_state( regs );

  workspace.ostask.irq_task = running;

  OSTask *resume = running->next;

  workspace.ostask.running = resume;

  // It can't be the only task in the list, there's always idle
  assert( running->next != running && running->prev != running );

  dll_detach_OSTask( running );

  // Removed properly from list
  assert( running->next == running && running->prev == running );
  assert( workspace.ostask.running != running );

  // Removing the head leaves the old next at the head
  assert( workspace.ostask.running == resume );

#if 0
  // Check the running list; all tasks must have the same interrupt enable
  // status.
  OSTask *t = resume;
  uint32_t status = t->regs.spsr & 0x80;
  do {
    t = t->next;
    if (status != (t->regs.spsr & 0x80)) PANIC;
  } while (t != resume);
#endif

  return resume;
}

app_memory_block block_containing( uint32_t va );

static inline
OSTask *TaskOpPhysicalFromVirtual( svc_registers *regs )
{
  uint32_t va = regs->r[0];
  uint32_t length = regs->r[1];

  app_memory_block block = block_containing( va );

  if (block.pages == 0) asm ( "bkpt 3" : : "r" (block.pages) );

  // TODO return error, or sin bin.
  if (block.pages == 0) PANIC;
  if (va - (block.va_page << 12) + length > (block.pages << 12)) PANIC;

  regs->r[0] = (block.page_base << 12) + va - (block.va_page << 12);

  push_writes_out_of_cache( va, length );

  return 0;
}

static inline
OSTask *TaskOpInvalidateCache( svc_registers *regs )
{
  uint32_t va = regs->r[0];
  uint32_t length = regs->r[1];

  RAM_may_have_changed( va, length );

  return 0;
}

static inline
OSTask *TaskOpFlushCache( svc_registers *regs )
{
  uint32_t va = regs->r[0];
  uint32_t length = regs->r[1];

  push_writes_out_of_cache( va, length );

  return 0;
}

static inline
OSTask *TaskOpSwitchToCore( svc_registers *regs )
{
  uint32_t core = regs->r[0];

#ifdef DEBUG__FOLLOW_TASKS
Task_LogString( "Switch ", 0 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogString( " from ", 0 );
Task_LogSmallNumber( workspace.core );
Task_LogString( " to ", 0 );
Task_LogSmallNumber( core );
Task_LogNewLine();
#endif

  if (core == workspace.core) return 0; // Optimisation!

  if (core > shared.ostask.number_of_cores) PANIC;

  // Calling this from a protected mode would move the task onto
  // a totally different stack. FIXME: Put the task in a "sin bin".
  if ((regs->spsr & 0x1f) != 0x10) PANIC;

  OSTask *running = workspace.ostask.running;

  save_task_state( regs );

  OSTask *resume = running->next;

  workspace.ostask.running = resume;

  // It can't be the only task in the list, there's always idle
  assert( running->next != running && running->prev != running );

  dll_detach_OSTask( running );

  // Removed properly from list
  assert( running->next == running && running->prev == running );
  assert( workspace.ostask.running != running );

  // Removing the head leaves the old next at the head
  assert( workspace.ostask.running == resume );

  // First come, first served (by each core)
  mpsafe_insert_OSTask_at_tail( &shared.ostask.moving, running );

  return resume;
}

__attribute__(( weak ))
OSTask *TaskOpRegisterSWIHandlers( svc_registers *regs )
{
  PANIC;
  return 0;
}

static inline
OSTask *TaskOpMapDevicePages( svc_registers *regs )
{
  uint32_t virt = regs->r[0];
  uint32_t page_base = regs->r[1];
  uint32_t pages = regs->r[2];
#ifdef DEBUG__LOG_SLOT_MEMORY
Task_LogString( "Mapping ", 0 );
Task_LogHex( page_base << 12 );
Task_LogString( " at ", 4 );
Task_LogHex( virt );
Task_LogString( " in ", 4 );
Task_LogHex( (uint32_t) workspace.ostask.running->slot );
Task_LogNewLine();
#endif
  regs->r[0] = map_device_pages( virt, page_base, pages );
  return 0;
}

// TODO: Make this more general? For use for dynamic areas, perhaps?
__attribute__(( noinline ))
OSTask *TaskOpMapFrameBuffer( svc_registers *regs )
{
  uint32_t phys = regs->r[0];
  uint32_t pages = regs->r[1];

#if 0
  send_number( phys, 'F' );
  send_number( pages, '\n' );
#endif
#if 0
  if ((pages & 0xff) != 0) PANIC; // TODO Deal with smaller or unaligned buffers
  if ((phys & 0xff) != 0) PANIC; // TODO Deal with smaller or unaligned buffers

  extern uint8_t frame_buffers_base;
  extern uint8_t frame_buffers_top;

  uint32_t fb_base = &frame_buffers_base - (uint8_t*)0;
  uint32_t fb_top = &frame_buffers_top - (uint8_t*)0;

  if (shared.ostask.frame_buffer_base == 0) {
    change_word_if_equal( &shared.ostask.frame_buffer_base, 0, fb_base );
    assert( shared.ostask.frame_buffer_base != 0 );
  }

  uint32_t base;
  uint32_t old = shared.ostask.frame_buffer_base;

  do {
    base = old;
    uint32_t next = base + (pages << 12);
    old = change_word_if_equal( &shared.ostask.frame_buffer_base, base, next );

    if (next >= fb_top) PANIC; // FIXME: Deal with running out of space
  } while (old != base);
#else
  uint32_t base = 0xc0000000;
#endif

  memory_mapping mapping = {
    .base_page = phys,
    .pages = pages,
    .va = base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &mapping );

  regs->r[0] = base;

  return 0;
}

static inline
OSTask *TaskOpAppMemoryTop( svc_registers *regs )
{
  regs->r[0] = app_memory_top( regs->r[0] );
  return 0;
}

OSTask *ostask_svc( svc_registers *regs, int number )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = 0;

  if (running->next == running
   && running != workspace.ostask.idle) PANIC;

  uint32_t swi = (number & ~Xbit); // A bit of RISC OS creeping in!

  switch (swi) {
  case OSTask_Yield:
    resume = TaskOpYield( regs );
    break;
  case OSTask_Sleep:
    resume = TaskOpSleep( regs );
    break;
  case OSTask_Create:
    resume = TaskOpCreate( regs, false );
    break;
  case OSTask_Spawn:
    resume = TaskOpCreate( regs, true );
    break;
  case OSTask_EndTask:
    resume = TaskOpEndTask( regs );
    break;
  case OSTask_RegisterSWIHandlers:
    resume = TaskOpRegisterSWIHandlers( regs );
    break;
  case OSTask_MapDevicePages:
    resume = TaskOpMapDevicePages( regs );
    break;
  case OSTask_AppMemoryTop:
    resume = TaskOpAppMemoryTop( regs );
    break;
  case OSTask_RunForTask:
    resume = TaskOpRunForTask( regs );
    break;
  case OSTask_GetRegisters:
    resume = TaskOpGetRegisters( regs );
    break;
  case OSTask_SetRegisters:
    resume = TaskOpSetRegisters( regs );
    break;
  case OSTask_Finished:
    resume = TaskOpFinished( regs );
    break;
  case OSTask_ReleaseTask:
    resume = TaskOpReleaseTask( regs );
    break;
  case OSTask_ChangeController:
    resume = TaskOpChangeController( regs );
    break;
  case OSTask_SetController:
    resume = TaskOpSetController( regs );
    break;
  case OSTask_Cores:
    {
    core_info result = { .current = workspace.core,
                         .total = shared.ostask.number_of_cores };
    regs->r[0] = result.raw;
    }
    break;
  case OSTask_LockClaim:
    resume = TaskOpLockClaim( regs );
    break;
  case OSTask_LockRelease:
    resume = TaskOpLockRelease( regs );
    break;
  case OSTask_EnablingInterrupts:
    regs->spsr |= 0x80;
    break;
  case OSTask_WaitForInterrupt:
    resume = TaskOpWaitForInterrupt( regs );
    break;
  case OSTask_PhysicalFromVirtual:
    resume = TaskOpPhysicalFromVirtual( regs );
    break;
  case OSTask_InvalidateCache:
    resume = TaskOpInvalidateCache( regs );
    break;
  case OSTask_FlushCache:
    resume = TaskOpFlushCache( regs );
    break;
  case OSTask_SwitchToCore:
    resume = TaskOpSwitchToCore( regs );
    break;
  case OSTask_Tick:
    // Woken tasks go into the runnable list
if (0) {
/*
char tmp[4] = "T #\n";
tmp[2] = '0' + workspace.core;
Task_LogString( tmp, 4 );
*/
  *((uint32_t*) 0xfffff000) = 'a' + workspace.core;
}
    sleeping_tasks_tick();
    break;
  case OSTask_MapFrameBuffer:
    resume = TaskOpMapFrameBuffer( regs );
    break;
  case OSTask_GetLogPipe:
    resume = TaskOpGetLogPipe( regs );
    break;
  case OSTask_LogString:
    resume = TaskOpLogString( regs );
    assert( resume == 0 );
    assert( 0 == (regs->spsr & VF) );
    break;
  case OSTask_PipeCreate ... OSTask_PipeCreate + 15:
    {
      bool reclaimed = core_claim_lock( &shared.ostask.pipes_lock,
                                        workspace.core+1 );
      assert( !reclaimed ); // I can't imagine a recursion situation

      if (swi == OSTask_PipeCreate) {
        resume = PipeCreate( regs );
      }
      else {
        OSPipe *pipe = pipe_from_handle( regs->r[0] );
        if (pipe == 0) {
          resume = Error_InvalidPipeHandle( regs );
        }
        else {
          switch (swi) {
          case OSTask_PipeWaitForSpace:
            resume = PipeWaitForSpace( regs, pipe ); break;
          case OSTask_PipeSpaceFilled:
            resume = PipeSpaceFilled( regs, pipe ); break;
          case OSTask_PipeSetSender:
            resume = PipeSetSender( regs, pipe ); break;
          case OSTask_PipeUnreadData:
            resume = PipeUnreadData( regs, pipe ); break;
          case OSTask_PipeNoMoreData:
            resume = PipeNoMoreData( regs, pipe ); break;
          case OSTask_PipeWaitForData:
            resume = PipeWaitForData( regs, pipe ); break;
          case OSTask_PipeDataConsumed:
            resume = PipeDataConsumed( regs, pipe ); break;
          case OSTask_PipeSetReceiver:
            resume = PipeSetReceiver( regs, pipe ); break;
          case OSTask_PipeNotListening:
            resume = PipeNotListening( regs, pipe ); break;
          case OSTask_PipeWaitUntilEmpty:
            // Not done yet... (For use by sender, by the way.)
            resume = Error_UnknownSWI( regs ); break;
          default:
            resume = Error_UnknownPipeSWI( regs ); break;
          }
        }
      }

      if (!reclaimed) core_release_lock( &shared.ostask.pipes_lock );
    }
    break;
  case OSTask_QueueCreate ... OSTask_QueueCreate + 16:
    {
      OSQueue *queue = 0;

      if (swi == OSTask_QueueCreate) {
        resume = QueueCreate( regs );
      }
      else {
        queue = queue_from_handle( regs->r[0] );
        if (queue == 0) {
          resume = Error_InvalidQueue( regs );
        }
        else {
          switch (swi) {
          case OSTask_QueueCreate:
            break;
          case OSTask_QueueWait:
            resume = QueueWait( regs, queue, false, false );
            break;
          case OSTask_QueueWaitSWI:
            resume = QueueWait( regs, queue, true, false );
            break;
          case OSTask_QueueWaitCore:
            resume = QueueWait( regs, queue, false, true );
            break;
          case OSTask_QueueWaitCoreAndSWI:
            resume = QueueWait( regs, queue, true, true );
            break;
          default:
            {
            resume = Error_UnknownQueueSWI( regs );
            }
          };
        }
      }
    }
    break;
  default: asm ( "bkpt 0xffff" );
  }

  // If running has been put into some shared queue by the SWI, it may already
  // have been picked up by another core. DO NOT make any more changes to it!

  return resume;
}

void __attribute__(( noreturn )) execute_svc( svc_registers *regs )
{
  regs->spsr &= ~VF;

  uint32_t number = get_svc_number( regs->lr );

  execute_swi( regs, number );

  __builtin_unreachable();
}

void __attribute__(( naked )) svc_handler()
{
  asm volatile (
      // This should detect if the C portion of this function gets
      // complex enough that the code generator needs stack.
      "\n.ifne .-svc_handler"
      "\n  .error \"svc_handler compiled code includes instructions before srsdb\""
      "\n.endif"

      "\n  srsdb sp!, #0x13 // Store return address and SPSR (SVC mode)"
      "\n  push { r0-r12 }  // and all the non-banked registers"
  );

  svc_registers *regs;
  asm volatile ( "mov %[regs], sp" : [regs] "=r" (regs) );

  execute_svc( regs );

  __builtin_unreachable();
}

// In case not supplied by another subsystem...
void __attribute__(( weak )) interrupting_privileged_code( OSTask *task )
{
  PANIC;
}

#define PROVE_OFFSET( o, t, n ) \
  while (((uint32_t) &((t *) 0)->o) != (n)) \
    asm ( "  .error \"Code assumes offset of " #o " in " #t " is " #n "\"" );

void __attribute__(( naked, noreturn )) irq_handler()
{
  asm volatile (
        "sub lr, lr, #4"
    "\n  srsdb sp!, #0x12 // Store return address and SPSR (IRQ mode)" );

  OSTask *interrupted_task;
  uint32_t interrupted_mode;

  {
    // Need to be careful with this, that the compiler doesn't insert any
    // code to set up lr using another register, corrupting it.
    PROVE_OFFSET( regs, OSTask, 0 );
    PROVE_OFFSET( lr, svc_registers, 13 * 4 );
    PROVE_OFFSET( spsr, svc_registers, 14 * 4 );
    PROVE_OFFSET( banked_sp_usr, OSTask, 15 * 4 );
    PROVE_OFFSET( banked_lr_usr, OSTask, 16 * 4 );

    register OSTask *volatile *head asm ( "lr" ) = &workspace.ostask.running;
    register OSTask *interrupted asm ( "lr" );
    register uint32_t mode asm ( "r1" );
    asm volatile (
        "\n  ldr lr, [lr]       // setting lr -> running->regs"
        "\n  stm lr!, {r0-r12}  // setting lr -> lr/spsr"
        "\n  pop { r0, r1 }     // Resume address, SPSR"
        "\n  stm lr, { r0, r1, sp, lr }^"
        "\n  sub lr, lr, #13*4 // restores its value"
        : "=r" (interrupted)
        , "=r" (mode)
        : "r" (head)
        ); // Calling task state saved, except for SVC sp & lr
    interrupted_task = interrupted;
    interrupted_mode = mode & 0x1f;
  }

  svc_registers *regs = &interrupted_task->regs;

  if (interrupted_mode != 0x10) {
    // Legacy RISC OS code allows for SWIs to be interrupted, this is
    // a Bad Thing. (IMO.)

    interrupting_privileged_code( interrupted_task );
  }

  OSTask *irq_task = workspace.ostask.irq_task;
  workspace.ostask.irq_task = 0;

  // The interrupted task stays on this core, this shouldn't take long!

  if (irq_task == 0) PANIC;

  // Sideline the interruptable tasks running on this core until the
  // interrupt tasks are done and the idle task runs (calls yield) again.
  // To ensure the idle task isn't interrupted while tasks are sidelined,
  // disable interrupts in the idle task.
  if (workspace.ostask.idle != workspace.ostask.running) {
    dll_detach_OSTask( workspace.ostask.idle );
    workspace.ostask.interrupted_tasks = workspace.ostask.running;
    workspace.ostask.running = workspace.ostask.idle;
  }

  // Disable interrupts until the idle task is re-entered with no other
  // interrupt tasks runnable.
  workspace.ostask.idle->regs.spsr |= 0x80;

  // So that the runnable list will be checked once the interrupts have
  // been handled, we make sure the idle task is run first.
  // Is this sufficient? Maybe the core should always pick up runnable
  // tasks until the list is empty before returning to the interrupted
  // task that may not sleep or yield much.
  workspace.ostask.running = workspace.ostask.idle;

  // New head of core's running list
  dll_attach_OSTask( irq_task, &workspace.ostask.running );

  // The interrupt task is always in OSTask_WaitForInterrupt

  map_slot( irq_task->slot );

  asm (
    "\n  msr sp_usr, %[usrsp]"
    "\n  msr lr_usr, %[usrlr]"
    :
    : [usrsp] "r" (irq_task->banked_sp_usr)
    , [usrlr] "r" (irq_task->banked_lr_usr)
  );

  register svc_registers *lr asm ( "lr" ) = &irq_task->regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

static void setup_processor_vectors()
{
  struct vectors {
    uint32_t reset; // ldr pc, ..._vec
    uint32_t undef;
    uint32_t svc;
    uint32_t prefetch;
    uint32_t data;
    uint32_t unused_vector;
    uint32_t irq;
    uint32_t fiq[1024 - 14];

    void *reset_vec;
    void *undef_vec;
    void *svc_vec;
    void *prefetch_vec;
    void *data_vec;
    void *unused; // Needed to keep the distance the same
    void *irq_vec;
  } *vectors = (void*) 0xffff0000;

  memory_mapping vector_page = {
    .base_page = claim_contiguous_memory( 0x1 ),
    .pages = 0x1,
    .va = 0xffff0000,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (vector_page.base_page == 0xffffffff) PANIC;
  map_memory( &vector_page );

  int32_t vector_offset = offset_of( struct vectors, reset_vec ) - 8;

  vectors->reset         = 0xe59ff000 + vector_offset; // ldr pc, ..._vec
  vectors->undef         = 0xe59ff000 + vector_offset;
  vectors->svc           = 0xe59ff000 + vector_offset;
  vectors->prefetch      = 0xe59ff000 + vector_offset;
  vectors->data          = 0xe59ff000 + vector_offset;
  vectors->unused_vector = 0xeafffffe; // for (;;) {}
  vectors->irq           = 0xe59ff000 + vector_offset;
  vectors->fiq[0]        = 0xeafffffe; // for (;;) {}

  vectors->reset_vec     = reset_handler;
  vectors->undef_vec     = undefined_instruction_handler;
  vectors->svc_vec       = svc_handler;
  vectors->prefetch_vec  = prefetch_handler;
  vectors->data_vec      = data_abort_handler;
  vectors->unused        = 0; // Needed to keep the distance the same
  vectors->irq_vec       = irq_handler;

  // Give all those modes a stack to work with
  asm ( "msr sp_und, %[stack]" : : [stack] "r" ((&(workspace.ostask.und_stack)) + 1) );
  asm ( "msr sp_abt, %[stack]" : : [stack] "r" ((&(workspace.ostask.abt_stack)) + 1) );
  asm ( "msr sp_irq, %[stack]" : : [stack] "r" ((&(workspace.ostask.irq_stack)) + 1) );
  asm ( "msr sp_fiq, %[stack]" : : [stack] "r" ((&(workspace.ostask.fiq_stack)) + 1) );
}

#ifdef DEBUG__GIVE_UP_ON_PREFETCH
struct OSPipe {
  OSPipe *next;
  OSPipe *prev;
  OSTask *sender;
  uint32_t sender_waiting_for; // Non-zero if blocked
  uint32_t sender_va; // Zero if not allocated
  OSTask *receiver;
  uint32_t receiver_waiting_for; // Non-zero if blocked
  uint32_t receiver_va; // Zero if not allocated

  uint32_t memory;      // In slot memory (including pipes?)
  OSTaskSlot *owner;    // NULL, if memory is owned by the pipe
  uint32_t max_block_size;
  uint32_t max_data;
  uint32_t write_index;
  uint32_t read_index;
};

OSTask *__attribute__(( noinline )) give_up_prefetch_handler( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

  bool reclaimed = lock_ostask();

  // Also includes Breakpoints.
  UART volatile * const uart = (void*) 0xfffff000;

  uart->data = '[';
  send_number( (uint32_t) running, ']' );
  uart->data = '\n';
  send_number( running->regs.lr, '\n' );

  uint32_t *p = &regs->r[0];
  for (int i = 0; i < 15; i++) {
    send_number( p[i], ' ' );
  }
  uart->data = '\n';

  send_number( (uint32_t) workspace.core, '-' );
  send_number( (uint32_t) workspace.ostask.running, '-' );
  send_number( (uint32_t) shared.ostask.runnable, '-' );
  send_number( (uint32_t) shared.ostask.blocked, '-' );
  send_number( (uint32_t) shared.ostask.moving, '-' );
  send_number( (uint32_t) shared.ostask.sleeping, '\n' );
  send_number( 0xff5ff9d8, ' ' );
  send_number( (uint32_t) ostask_from_handle( *(uint32_t*) 0xff5ff9d8 ), '\n' );

  {
OSPipe volatile *p = shared.ostask.pipes;
  if (p != 0) {
    do {
      send_number( (uint32_t) p, 'p' );
      send_number( p->write_index, 'w' );
      send_number( p->read_index, 'r' );
      send_number( p->receiver_waiting_for, ' ' );
      send_number( (uint32_t) p->receiver, ' ' );
      send_number( p->sender_waiting_for, 's' );
      send_number( (uint32_t) p->sender, 'v' );

      send_number( p->sender_va, ' ' );
      send_number( p->receiver_va, '\n' );

      if (p->write_index != p->read_index) {
        send_number( p->write_index - p->read_index, '\n' );
        //if (p->receiver == running) {
        if (p->receiver != 0 && p->receiver->slot == running->slot) {
          // Is receiver

      extern uint32_t translation_table[];
      extern uint32_t VMSAv6_Level2_Tables;
      uint32_t pl2tt = translation_table[p->receiver_va >> 20] & ~0x3ff;
      send_number( translation_table[p->receiver_va >> 20], ',' );
      uint32_t vl2tt = (pl2tt & 0xffc00) + ((uint32_t) &VMSAv6_Level2_Tables);
      uint32_t *l2tt = (void*) vl2tt;
      send_number( l2tt[(p->receiver_va >> 12) & 0xff], '\n' );

          uint8_t *s = ((uint8_t*) (p->receiver_va + p->read_index));
          for (int i = 0; i < (p->write_index - p->read_index); i++) {
            send_char( s[i] );
          }
          send_char( '\n' );
          s = ((uint8_t*) (p->receiver_va));
          for (int i = 0; i < (p->write_index & 0xfff); i++) {
            send_char( s[i] );
          }
          send_char( '\n' );
        }
        if (p->sender == running || p == workspace.ostask.log_pipe) {
        //if (p->sender != 0 && p->sender->slot == running->slot) {
          // Is sender

      extern uint32_t translation_table[];
      extern uint32_t VMSAv6_Level2_Tables;
      uint32_t pl2tt = translation_table[p->sender_va >> 20] & ~0x3ff;
      send_number( translation_table[p->sender_va >> 20], ',' );
      uint32_t vl2tt = (pl2tt & 0xffc00) + ((uint32_t) &VMSAv6_Level2_Tables);
      uint32_t *l2tt = (void*) vl2tt;
      send_number( l2tt[(p->sender_va >> 12) & 0xff], '\n' );

          uint8_t *s = ((uint8_t*) (p->sender_va + p->read_index));
          for (int i = 0; i < (p->write_index - p->read_index); i++) {
            send_char( s[i] );
          }
          send_char( '\n' );
          s = ((uint8_t*) (p->sender_va));
          for (int i = 0; i < (p->write_index & 0xfff); i++) {
            send_char( s[i] );
          }
          send_char( '\n' );
        }
      }
      p = p->next;
    } while (p != shared.ostask.pipes);
  }
  }

  p = (void*) &OSTask_free_pool;

  for (int i = 0; i < 28; i++) {
    send_number( 0xff100000 + i * 88, ' ' );
    for (int t = 0; t < 22; t++) {
      send_number( p[i * 22 + t], t == 21 ? '\n' : ' ' );
    }
  }

  for (;;) {}

  // Doesn't matter if reclaimed, let other cores have a go
  core_release_lock( &shared.ostask.lock );

  // For now, just cut the task loose...
  // TODO: Put it in a queue to be handled (call signal code, etc.)

  assert( resume != running );

  dll_detach_OSTask( running );

  if (0 == (resume->regs.spsr & 15)) {
    asm ( "msr sp_usr, %[sp]"
      "\n  msr lr_usr, %[lr]"
        :
        : [sp] "r" (resume->banked_sp_usr)
        , [lr] "r" (resume->banked_lr_usr) );
  }

  map_slot( resume->slot );

  return resume;
}

void __attribute__(( naked )) prefetch_handler()
{
  // Tasks with a prefetch exception cannot continue to run without
  // help, so don't bother storing the state on the stack, drop it
  // straight into the OSTask...

  asm volatile (
    "\n.ifne .-prefetch_handler"
    "\n  .error \"prefetch_handler compiled code includes instructions before srsdb\""
    "\n.endif"
    "\n  srsdb sp!, #0x17 // Store fail address and SPSR (Abt mode)"
    "\n  stmdb sp!, { r0-r12 }"
    "\n  mov r0, sp"
    "\n  b give_up_prefetch_handler"
  );

  __builtin_unreachable();
}
#else
// Note: Different parameters to the "give up" version.
OSTask *__attribute__(( noinline )) c_prefetch_handler( OSTask *running )
{
  // Report breakpoints to something
  // Queue the (no-longer) running task to be saved or killed.
  for (;;) { asm ( "wfi" ); }
  PANIC;

  OSTask *resume = 0;
  assert( resume != running );

  dll_detach_OSTask( running );

  if (0 == (resume->regs.spsr & 15)) {
    asm ( "msr sp_usr, %[sp]"
      "\n  msr lr_usr, %[lr]"
        :
        : [sp] "r" (resume->banked_sp_usr)
        , [lr] "r" (resume->banked_lr_usr) );
  }

  map_slot( resume->slot );

  return resume;
}

void __attribute__(( naked )) prefetch_handler()
{
  // Tasks with a prefetch exception cannot continue to run without
  // help, so don't bother storing the state on the stack, drop it
  // straight into the OSTask...

  asm volatile (
    "\n.ifne .-prefetch_handler"
    "\n  .error \"prefetch_handler check generated code\""
    "\n.endif"
    "\n  srsdb sp!, #0x17 // Store fail address and SPSR (Abt mode)"
  );

  register OSTask *volatile *head asm ( "lr" ) = &workspace.ostask.running;
  register OSTask *running;

  asm volatile (
    "\n.ifne .-prefetch_handler-8" // 1 instruction to set lr
    "\n  .error \"prefetch_handler check generated code\""
    "\n.endif"
    "\n  ldr lr, [lr]"
    "\n  stmia lr!, {r0-r12}"
    "\n  ldm sp!, {r0-r1}"
    "\n  mrs r2, sp_usr"
    "\n  mrs r3, lr_usr"
    "\n  stmia lr!, {r0-r3}"
    "\n  sub %[running], lr, %[off]"
    : [running] "=r" (running)
    , "=r" (head) // Corrupted
    : "r" (head)
    , [off] "i" (offset_of( OSTask, slot )) );

  if (running != workspace.ostask.running) {
    running = ((uint32_t) workspace.ostask.running - (uint32_t) running);
  }
  workspace.ostask.running = c_prefetch_handler( running );

  register svc_registers *lr asm ( "lr" ) = &workspace.ostask.running->regs;

  asm (
    "\n  msr sp_usr, %[usrsp]"
    "\n  msr lr_usr, %[usrlr]"
    :
    : [usrsp] "r" (workspace.ostask.running->banked_sp_usr)
    , [usrlr] "r" (workspace.ostask.running->banked_lr_usr)
  );

  asm (
    "\n  ldm lr!, {r0-r12}"
    "\n  rfeia lr // Restore execution and SPSR"
    :
    : "r" (lr) );

  __builtin_unreachable();
}
#endif

 __attribute__(( noinline, noreturn ))
void signal_data_abort( svc_registers *regs, uint32_t fa, uint32_t ft )
{
  // This routine horribly assumes a load of stuff, but will be replaced
  // by a much better system TODO

  send_number( (uint32_t) workspace.ostask.running, ':' );
  send_number( fa, ' ' );
  send_number( ft, ' ' );
  send_number( workspace.core, '\n' );

  register uint32_t r;
  asm ( "mrs %[r], lr_usr" : [r] "=r" (r) );
  send_number( r, '\n' );

    for (int t = 0; t < 22; t++) {
      uint32_t *p = &regs->r[0];
      send_number( p[t], t == 21 ? '\n' : ' ' );
    }

  extern uint32_t translation_table[];

  uint32_t l1 = translation_table[fa >> 20];
  send_number( l1, '\n' );

  uint32_t *p = (void*) &OSTask_free_pool;

  for (int i = 0; i < 16; i++) {
    for (int t = 0; t < 22; t++) {
      send_number( p[i * 22 + t], t == 21 ? '\n' : ' ' );
    }
  }

asm ( "bkpt 1" );
  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

  if (0 == (resume->regs.spsr & 15)) {
      asm ( "msr sp_usr, %[sp]"
        "\n  msr lr_usr, %[lr]"
          :
          : [sp] "r" (resume->banked_sp_usr)
          , [lr] "r" (resume->banked_lr_usr) );
  }

  map_slot( resume->slot );

  assert( running != running->next );
  dll_detach_OSTask( running );

  workspace.ostask.running = resume;

  // Reset the abort stack pointer
  asm (
      "add sp, %[regs], %[size]"
      :
      : [regs] "r" (regs)
      , [size] "i" (sizeof( svc_registers )) );

  register svc_registers *lr asm ( "lr" ) = &resume->regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

// Needed for controllers array in ostask.h
void *memmove(void *dest, const void *src, size_t n)
{
  uint32_t *d = dest;
  uint32_t const *s = src;
  if (d != s) {
    if (d < s)
      for (int i = 0; i < n/4; i++) d[i] = s[i];
    else
      for (int i = (n/4)-1; i >= 0; i--) d[i] = s[i];
  }
  return dest;
}

