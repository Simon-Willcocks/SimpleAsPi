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

#define assert( x ) do { if (!(x)) PANIC; } while (false)

// OK, to start with, temporarily, 1MiB sections
extern OSTask OSTask_free_pool[];
extern OSTaskSlot OSTaskSlot_free_pool[];

MPSAFE_DLL_TYPE( OSTaskSlot );

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
  forget_boot_low_memory_mapping();

  bool reclaimed = core_claim_lock( &shared.ostask.lock, core + 1 );
  if (reclaimed) PANIC;

  bool first = (shared.ostask.first == 0);

  workspace.core = core;

  if (first)
  {
    shared.ostask.number_of_cores = number_of_cores();

    extern mmu_page global_devices_top;
    shared.ostask.last_global_device = &global_devices_top;

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

  // Make this running code into an OSTask
  workspace.ostask.running = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );
  workspace.ostask.running->slot = shared.ostask.first;

  map_slot( workspace.ostask.running->slot );

  if (first) {
    // Create a separate idle OSTask
    workspace.ostask.idle = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );
    workspace.ostask.idle->regs.lr = (uint32_t) idle_task;

    uint32_t cpsr;
    asm ( "mrs %[cpsr], cpsr" : [cpsr] "=r" (cpsr) );

    // usr32, interrupts enabled
    workspace.ostask.idle->regs.spsr = cpsr & ~0x1ef;
    workspace.ostask.idle->slot = shared.ostask.first;

    dll_attach_OSTask( workspace.ostask.idle, &workspace.ostask.running );
    workspace.ostask.running = workspace.ostask.running->next;

    startup();
  }
  else {
    // Become the idle OSTask
    workspace.ostask.idle = workspace.ostask.running;
    workspace.ostask.idle->slot = shared.ostask.first;

    // Before we start scheduling tasks from other cores, ensure the shared
    // memory areas that memory_fault_handlers might use are mapped into this
    // core's translation tables. (The first core in already has them.)

    // This is not the prettiest approach, but I think it should work.
    asm ( "ldr r0, [%[mem]]" : : [mem] "r" (&OSTaskSlot_free_pool) );

    asm ( "mov sp, %[reset_sp]"
      "\n  cpsid aif, #0x10"
      :
      : [reset_sp] "r" ((&workspace.svc_stack)+1) );

    idle_task();
  }

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) idle_task()
{
  // Yield will usually check for other OSTasks that want to be run
  // and schedule one or return immediately. When called by the core's
  // idle OSTask, it will not return until an OSTask has become runnable
  // (and that one yields or blocks).
  asm ( "svc %[yield]"
    "\n  b idle_task"
    :
    : [yield] "i" (OSTask_Yield)
    , "r" (0x44) );
}

void __attribute__(( naked )) reset_handler()
{
  PANIC;
}

void __attribute__(( naked )) undefined_instruction_handler()
{
  // Return to the next instruction. It's useful as a debugging tool
  // to insert asm ( "udf 1" ) to see the registers at that point.
  asm volatile (
        "srsdb sp!, #0x1b // Store return address and SPSR (UND mode)"
    "\n  rfeia sp! // Restore execution and SPSR"
      );
}

void __attribute__(( noinline )) save_task_state( svc_registers *regs )
{
  workspace.ostask.running->regs = *regs;
#ifdef DEBUG__UDF_ON_SAVE_TASK_STATE
  asm ( "udf 3" );
#endif
  if (workspace.ostask.running->regs.lr == 0) PANIC;
}

void unexpected_task_return()
{
  PANIC;
}

static inline void *put_to_sleep( OSTask **head, void *p )
{
  OSTask *tired = p;
  uint32_t time = tired->regs.r[0];

  OSTask *t = *head;
  if (t == 0) {
    *head = tired;
  }
  else {
    if (t->regs.r[0] > time) {
      // Before the first queued task
      t->regs.r[0] -= time;
      dll_attach_OSTask( tired, head );
    }
    else {
      for (;;) {
        if (t->regs.r[0] <= time) {
          time -= t->regs.r[0];
          t = t->next;
          // Any need to look further?
          if (time == 0) break;
          // Is t the last entry in the list?
          if (t->next == *head) break;
        }
        else {
          // t's going to be waiting longer
          t->regs.r[0] -= time;
          break;
        }
      }
      tired->regs.r[0] = time;
      // Insert before t (which may be *head, in which case
      // this will be the last item in the list)
      dll_attach_OSTask( tired, &t );
    }
  }

  return 0;
}

static inline void *wakey_wakey( OSTask **headptr, void *p )
{
  p = p;

  OSTask *head = *headptr;
  OSTask *t = head;

  if (t == 0) return 0;
  if (0 < --t->regs.r[0]) return 0;

  OSTask *end = t;

  do {
    end = t;
    t = t->next;
  } while (t->regs.r[0] == 0 && t != head);

  dll_detach_OSTasks_until( headptr, end );

  return head;
}

static inline void *add_woken( OSTask **headptr, void *p )
{
  dll_insert_OSTask_list_at_head( p, headptr );

  return 0;
}

static void sleeping_tasks_add( OSTask *tired )
{
  mpsafe_manipulate_OSTask_list( &shared.ostask.sleeping, put_to_sleep, tired );
}

static void sleeping_tasks_tick()
{
  OSTask *list = mpsafe_manipulate_OSTask_list( &shared.ostask.sleeping, wakey_wakey, 0 );
  if (list != 0)
    mpsafe_manipulate_OSTask_list( &shared.ostask.runnable, add_woken, list );
}

DEFINE_ERROR( UnknownSWI, 0x1e6, "Unknown SWI" );
DEFINE_ERROR( UnknownPipeSWI, 0x888, "Unknown Pipe operation" );
DEFINE_ERROR( InvalidPipeHandle, 0x888, "Invalid Pipe handle" );
DEFINE_ERROR( InvalidQueue, 0x888, "Invalid OSTask Queue handle" );
DEFINE_ERROR( UnknownQueueSWI, 0x888, "Unknown Queue operation" );

DEFINE_ERROR( NotATask, 0x666, "Programmer error: Not a task" );
DEFINE_ERROR( NotYourTask, 0x666, "Programmer error: Not your task" );

static OSTask *RunInControlledTask( svc_registers *regs, bool wait )
{
  // This implementation depends on the shared.ostask.lock being held by this core

  OSTask *running = workspace.ostask.running;
  OSTask *release = ostask_from_handle( regs->r[0] );
  svc_registers *context = (void*) regs->r[1];

  if (release == 0) {
    return Error_NotATask( regs );
  }

  if (release->controller != running) {
    return Error_NotYourTask( regs );
  }

  if (context != 0) {
    release->regs = *context;
  }

  release->controller = 0;

  // This must be done before the release Task is known to be runnable
  if (wait) {
    save_task_state( regs );
    workspace.ostask.running = running->next;
    assert( running != running->next );
    dll_detach_OSTask( running );
    running->controller = release;
    // Might as well run it on this core, the running task just unlinked
    // itself.
    dll_attach_OSTask( release, &workspace.ostask.running );

    return release;
  }
  else {
    // The caller's going to keep running, let one of the other cores
    // pick up the resumed task.
    mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, release );

    return 0;
  }
}

OSTask *TaskOpRunThisForMe( svc_registers *regs )
{
  return RunInControlledTask( regs, true );
}

OSTask *TaskOpReleaseTask( svc_registers *regs )
{
  return RunInControlledTask( regs, false );
}

OSTask *TaskOpChangeController( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *release = ostask_from_handle( regs->r[0] );
  if (release == 0) {
    PANIC;
  }
  else {
    if (release->controller != running) {
      PANIC;
    }
    OSTask *new_controller = ostask_from_handle( regs->r[1] );
    if (new_controller == 0) PANIC;
    release->controller = new_controller;
  }
  return 0;
}

OSTask *TaskOpLockClaim( svc_registers *regs );
OSTask *TaskOpLockRelease( svc_registers *regs );

OSTask *TaskOpRelinquishControl( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;

  if (running->controller != 0) PANIC;

  running->controller = ostask_from_handle( regs->r[0] );
  if (0 == running->controller) {
    return Error_NotATask( regs );
  }

  save_task_state( regs );
  workspace.ostask.running = running->next;
  dll_detach_OSTask( running );

  OSTask *resume = 0;

  if (running->controller != 0) {
    resume = running->controller;

    if (resume->controller != running) PANIC;

    // Task is waiting, detached from the running list
    // Place at head of this core's running list (replacing
    // running).

    assert( resume->next == resume );
    assert( resume->prev == resume );

    resume->controller = 0;

    dll_attach_OSTask( resume, &workspace.ostask.running );
  }
  else {
    resume = running->next;
  }

  return resume;
}

OSTask *TaskOpGetRegisters( svc_registers *regs )
{
  OSTask *controlled = ostask_from_handle( regs->r[0] );
  svc_registers *context = (void*) regs->r[1];
  OSTask *running = workspace.ostask.running;

  if (controlled == 0) {
    return Error_NotATask( regs );
  }
  if (controlled->controller != running) {
    return Error_NotYourTask( regs );
  }

  *context = controlled->regs;

  return 0;
}

OSTask *TaskOpSetRegisters( svc_registers *regs )
{
  OSTask *controlled = ostask_from_handle( regs->r[0] );
  svc_registers *context = (void*) regs->r[1];
  OSTask *running = workspace.ostask.running;

  if (controlled == 0) {
    return Error_NotATask( regs );
  }
  if (controlled->controller != running) {
    return Error_NotYourTask( regs );
  }

  controlled->regs = *context;

  return 0;
}

OSTask *TaskOpSleep( svc_registers *regs, uint32_t ticks )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

  if (resume == running
   && running != workspace.ostask.idle) PANIC;

  if (resume == running) {
    if (ticks != 0) PANIC; // idle task never sleeps, only yields

    if (running != workspace.ostask.idle) PANIC;

    resume = mpsafe_detach_OSTask_at_head( &shared.ostask.runnable );

    if (resume != 0) {
      save_task_state( regs );

      dll_attach_OSTask( resume, &workspace.ostask.running );
      // Now resume and idle are in the list
    }
    else {
      // Pause, then drop back to idle task with interrupts enabled,
      // after which the runnable list will be checked again.
      //wait_for_event();
    }
  }
  else {
    save_task_state( regs );

    workspace.ostask.running = resume;

    // It can't be the only task in the list, there's always idle
    if (running->next == running || running->prev == running) PANIC;

    dll_detach_OSTask( running );

    // Removed properly from list
    if (running->next != running || running->prev != running) PANIC;
    if (workspace.ostask.running == running) PANIC;

    // Removing the head leaves the old next at the head
    if (workspace.ostask.running != resume) PANIC;

    if (ticks == 0) {
      // Instantly runnable, but let other tasks run
      mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, running );

      signal_event(); // Other cores should check runnable list, if waiting
    }
    else {
      sleeping_tasks_add( running );
    }
  }

  return resume;
}

OSTask *TaskOpCreate( svc_registers *regs, bool spawn )
{
  OSTask *running = workspace.ostask.running;
  OSTask *task = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );

  if (task->next != task || task->prev != task) PANIC;

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
  task->regs.spsr = 0x10;
  task->regs.r[0] = ostask_handle( task );
  task->regs.r[1] = regs->r[2];
  task->regs.r[2] = regs->r[3];
  task->regs.r[3] = regs->r[4];
  task->regs.r[4] = regs->r[5];

  // Keep the new task on the current core, in case it's a device
  // driver than needs it. Any Sleep or Yield indicates that the
  // task doesn't care what core it runs on.
  // TODO: decide whether blocking on a lock should maintain core
  // or not.
  OSTask *next = running->next;
  dll_attach_OSTask( task, &next );

  return 0;
}

OSTask *TaskOpEndTask( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = running->next;

  workspace.ostask.running = resume;

  // It can't be the only task in the list, there's always idle
  if (running->next == running || running->prev == running) PANIC;

  dll_detach_OSTask( running );

  // Removed properly from list
  if (running->next != running || running->prev != running) PANIC;
  if (workspace.ostask.running == running) PANIC;

  // Removing the head leaves the old next at the head
  if (workspace.ostask.running != resume) PANIC;

  // FIXME: Should call some higher lever housekeeping functions.

  // If this takes too long, the pointer could be sent to a
  // task that cleans it out and returns it to the pool.
  memset( running, 0, sizeof( OSTask ) );

  dll_new_OSTask( running );

  mpsafe_insert_OSTask_at_tail( &shared.ostask.task_pool, running );

  if (--running->slot->number_of_tasks == 0) {
    // FIXME
    PANIC;
  }

  return resume;
}

DEFINE_ERROR( NotHAL, 0, "Only the HAL is entitled to map global devices" );

OSTask *TaskOpMapDeviceGlobal( svc_registers *regs )
{
  // Request global privileged access to a device page.

  workspace.ostask.irq_task = workspace.ostask.running;

  if (shared.ostask.last_global_device == 0) {
    return Error_NotHAL( regs );
  }

  memory_mapping map = {
    .base_page = regs->r[0],
    .pages = 1,
    .vap = --shared.ostask.last_global_device,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &map );
  regs->r[0] = map.va;

  return 0;
}

OSTask *TaskOpWaitForInterrupt( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;

  if (0 == (regs->spsr & 0x80)) PANIC; // Must always have interrupts disabled
  if (workspace.ostask.irq_task != 0) PANIC;

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

  return resume;
}

OSTask *TaskOpSwitchToCore( svc_registers *regs )
{
  uint32_t core = regs->r[0];

  if (core > shared.ostask.number_of_cores) PANIC;

  OSTask *running = workspace.ostask.running;

  save_task_state( regs );

  if (shared.ostask.for_core == 0) {
    bool reclaimed = lock_ostask();
    assert ( !reclaimed );

    if (shared.ostask.for_core == 0) {
      uint32_t size = shared.ostask.number_of_cores * sizeof( OSTask * );
      OSTask **mem = shared_heap_allocate( size );
      shared.ostask.for_core = mem;
      memset( mem, 0, size );
    }

    release_ostask();
  }

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

  mpsafe_insert_OSTask_at_tail( &shared.ostask.for_core[core], running );

  return resume;
}

__attribute__(( weak ))
OSTask *TaskOpRegisterSWIHandlers( svc_registers *regs )
{
  PANIC;
  return 0;
}

OSTask *TaskOpMapDevicePages( svc_registers *regs )
{
  uint32_t virt = regs->r[0];
  uint32_t page_base = regs->r[1];
  uint32_t pages = regs->r[2];
  regs->r[0] = map_device_pages( virt, page_base, pages );
  return 0;
}

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

  switch (number) {
  case OSTask_Yield:
    resume = TaskOpSleep( regs, 0 );
    break;
  case OSTask_Sleep:
    resume = TaskOpSleep( regs, regs->r[0] );
    break;
  case OSTask_Spawn:
  case OSTask_Create:
    resume = TaskOpCreate( regs, (number == OSTask_Spawn) );
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
  case OSTask_RunThisForMe:
    resume = TaskOpRunThisForMe( regs );
    break;
  case OSTask_GetRegisters:
    resume = TaskOpGetRegisters( regs );
    break;
  case OSTask_SetRegisters:
    resume = TaskOpSetRegisters( regs );
    break;
  case OSTask_RelinquishControl:
    resume = TaskOpRelinquishControl( regs );
    break;
  case OSTask_ReleaseTask:
    resume = TaskOpReleaseTask( regs );
    break;
  case OSTask_ChangeController:
    resume = TaskOpChangeController( regs );
    break;
  case OSTask_GetTaskHandle:
    regs->r[0] = ostask_handle( running );
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
  case OSTask_MapDeviceGlobal:
    resume = TaskOpMapDeviceGlobal( regs );
    break;
  case OSTask_EnablingInterrupts:
    regs->spsr |= 0x80;
    break;
  case OSTask_WaitForInterrupt:
    resume = TaskOpWaitForInterrupt( regs );
    break;
  case OSTask_SwitchToCore:
    resume = TaskOpSwitchToCore( regs );
    break;
  case OSTask_Tick:
    sleeping_tasks_tick();
    break;
  case OSTask_PipeCreate ... OSTask_PipeCreate + 15:
    {
      bool reclaimed = core_claim_lock( &shared.ostask.pipes_lock,
                                        workspace.core+1 );
      if (reclaimed) PANIC; // I can't imagine a recursion situation

      if (number == OSTask_PipeCreate) {
        resume = PipeCreate( regs );
      }
      else {
        OSPipe *pipe = pipe_from_handle( regs->r[0] );
        if (pipe == 0) {
          resume = Error_InvalidPipeHandle( regs );
        }
        else {
          switch (number) {
          case OSTask_PipeCreate:
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

      if (number == OSTask_QueueCreate) {
        resume = QueueCreate( regs );
      }
      else {
        queue = queue_from_handle( regs->r[0] );
        if (queue == 0) {
          resume = Error_InvalidQueue( regs );
        }
        else {
          switch (number) {
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
  }

  if (resume != 0 && resume != workspace.ostask.running) PANIC;

// REMOVE!!! (Up to 3 tasks, one must be idle)
if (workspace.ostask.running != workspace.ostask.idle
 && workspace.ostask.running->next != workspace.ostask.idle
 && workspace.ostask.running->prev != workspace.ostask.idle) asm( "wfe" );

  // If running has been put into some shared queue by the SWI, it may already
  // have been picked up by another core. DO NOT make any more changes to it!

  return resume;
}

void NORET execute_svc( svc_registers *regs )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = 0;

  if (0 == (regs->spsr & 15)) {
    asm ( "mrs %[sp], sp_usr"
      "\n  mrs %[lr], lr_usr"
        : [sp] "=r" (running->banked_sp_usr)
        , [lr] "=r" (running->banked_lr_usr) );
  }

  uint32_t number = get_svc_number( regs->lr );
  uint32_t swi = (number & ~Xbit); // A bit of RISC OS creeping in!

  switch (swi) {
  case OSTask_Yield ... OSTask_Yield + 63:
    resume = ostask_svc( regs, number );
    break;
  default:
    resume = execute_swi( regs, number );
    break;
  }

  if (workspace.ostask.running->next == workspace.ostask.running
   && workspace.ostask.running != workspace.ostask.idle) PANIC;

  if (resume != 0) {
    if (0 == (resume->regs.spsr & 15)) {
      asm ( "msr sp_usr, %[sp]"
        "\n  msr lr_usr, %[lr]"
          :
          : [sp] "r" (resume->banked_sp_usr)
          , [lr] "r" (resume->banked_lr_usr) );
    }

    map_slot( resume->slot );
  }

  // Restore the stack pointer to where it was before the SVC
  // (nothing is going to use it; interrupts are disabled.)
  // (Make no function calls after this point!)

  asm (
      "add sp, %[regs], %[size]"
      :
      : [regs] "r" (regs)
      , [size] "i" (sizeof( svc_registers )) );

  if (resume != 0) {
    regs = &resume->regs;
  }

  // Resume after the SWI
  register svc_registers *lr asm ( "lr" ) = regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

void __attribute__(( naked )) svc_handler()
{
  asm volatile (
      // This should detect if the C portion of this function gets
      // complex enough that the code generator needs stack.
      "\n.ifne .-svc_handler"
      "\n  .error \"Kernel_default_svc compiled code includes instructions before srsdb\""
      "\n.endif"

      "\n  srsdb sp!, #0x13 // Store return address and SPSR (SVC mode)"
      "\n  push { r0-r12 }  // and all the non-banked registers"
  );

  svc_registers *regs;
  asm volatile ( "mov %[regs], sp" : [regs] "=r" (regs) );

  regs->spsr &= ~VF;
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

  {
    // Need to be careful with this, that the compiler doesn't insert any code to
    // set up lr using another register, corrupting it.
    PROVE_OFFSET( regs, OSTask, 0 );
    PROVE_OFFSET( lr, svc_registers, 13 * 4 );
    PROVE_OFFSET( spsr, svc_registers, 14 * 4 );
    PROVE_OFFSET( banked_sp_usr, OSTask, 15 * 4 );
    PROVE_OFFSET( banked_lr_usr, OSTask, 16 * 4 );

    register OSTask **running asm ( "lr" ) = &workspace.ostask.running;
    asm volatile (
        "\n  ldr lr, [lr]       // lr -> running"
        "\n  stm lr!, {r0-r12}  // lr -> lr/spsr"
        "\n  pop { r0, r1 }     // Resume address, SPSR"
        "\n  stm lr!, { r0, r1 }// lr -> banked_sp_usr"
        "\n  tst r1, #0xf"
        "\n  stmeq lr, { sp, lr }^ // Does not update lr, so..."
        "\n  sub %[task], lr, #15*4 // restores its value, in either case"
        : [task] "=r" (interrupted_task)
        , "=r" (running)
        : "r" (running)
        ); // Calling task state saved, except for SVC sp & lr
    asm volatile ( "" : : "r" (running) );
    // lr really, really used (Oh! Great Optimiser, please don't assume
    // it's still pointing to workspace.ostask.running, I beg you!)
  }

  svc_registers *regs = &interrupted_task->regs;
  uint32_t interrupted_mode = regs->spsr & 0x1f;

  if (interrupted_mode != 0x10) interrupting_privileged_code( interrupted_task );
  // Legacy RISC OS code allows for SWIs to be interrupted, this is
  // a Bad Thing.

  OSTask *irq_task = workspace.ostask.irq_task;
  workspace.ostask.irq_task = 0;

  // The interrupted task stays on this core, this shouldn't take long!

  if (irq_task == 0) PANIC;

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
