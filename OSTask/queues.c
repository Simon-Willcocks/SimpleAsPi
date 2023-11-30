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
#include "raw_memory_manager.h"

typedef union OSQueue OSQueue;

union __attribute__(( packed, aligned( 4 ) )) OSQueue {
  struct {
    OSTask *queue;
    OSTask *handlers;
  };
  struct {
    OSQueue *next;
    OSQueue *prev;
  };
};

// Only queue items while in the free pool; remember to do new_OSQueue
// before returning to the pool.
MPSAFE_DLL_TYPE( OSQueue );

DEFINE_ERROR( QueueCreationProblem, 0x888, "OSTask Queue creation problem" );

extern OSQueue OSQueue_free_pool[];

void setup_queue_pool()
{
  // FIXME: free the size from the 64KiB limit
  memory_mapping queues = {
    .base_page = claim_contiguous_memory( 0x10 ), // 64 KiB
    .pages = 0x10,
    .vap = &OSQueue_free_pool,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (queues.base_page == 0xffffffff) PANIC;
  map_memory( &queues );
  for (int i = 0; i < 0x10000 / sizeof( OSQueue ); i++) {
    OSQueue *q = &OSQueue_free_pool[i];
    memset( q, 0, sizeof( OSQueue ) );
    dll_new_OSQueue( q );
    dll_attach_OSQueue( q, &shared.ostask.queue_pool );
    shared.ostask.queue_pool = shared.ostask.queue_pool->next;
  }
}

uint32_t new_queue()
{
  OSQueue *queue = mpsafe_detach_OSQueue_at_head( &shared.ostask.queue_pool );

  if (queue == 0) {
    return 0;
  }

  queue->handlers = 0;
  queue->queue = 0;

  return queue_handle( queue );
}

OSTask *QueueCreate( svc_registers *regs )
{
  uint32_t handle = new_queue();
  OSQueue *queue = queue_from_handle( handle );

  if (queue == 0) {
    return Error_QueueCreationProblem( regs );
  }

  regs->r[0] = handle;

  return 0;
}

OSTask *QueueWait( svc_registers *regs, OSQueue *queue,
                   bool swi, bool core )
{
  // Wait for any queued OSTask

  // TODO validation, scan
  if (swi || core) PANIC;

  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;
  OSTask *result = 0;

  if (next == running) PANIC;

  // TODO: lock per queue?
  bool reclaimed = core_claim_lock( &shared.ostask.queues_lock,
                                    workspace.core + 1 );

  if (reclaimed) PANIC;

  if (queue->queue == 0) {
    // Nothing to remove, block caller
    save_task_state( regs );
    workspace.ostask.running = next;
    dll_detach_OSTask( running );
    dll_attach_OSTask( running, &queue->handlers );
    result = next;
  }
  else {
    // TODO scan queue for matching swi and/or core, if required
    // For now, just return the head

    OSTask *head = queue->queue;
    queue->queue = head->next;
    if (queue->queue == head) {
      // Single item queue
      queue->queue = 0;
    }
    else {
      dll_detach_OSTask( head );
    }
    regs->r[0] = ostask_handle( head );
    regs->r[1] = head->swi.offset;
    regs->r[2] = head->swi.core;
    head->controller = running;
  }

  core_release_lock( &shared.ostask.queues_lock );

  return result;
}

DECLARE_ERROR( InvalidQueue );

OSTask *queue_running_OSTask( svc_registers *regs, uint32_t queue_handle, uint32_t SWI )
{
  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;
  OSTask *result = next;

  OSQueue *queue = queue_from_handle( queue_handle );

  if (queue == 0) {
    return Error_InvalidQueue( regs );
  }

  // Whatever happens, the caller stops running
  // It will either be added to the queue, or relinquish control
  // to a handler OSTask.
  save_task_state( regs );
  workspace.ostask.running = next;
  dll_detach_OSTask( running );

  int op = SWI;
  int core = workspace.core;

  bool reclaimed = core_claim_lock( &shared.ostask.queues_lock,
                                    workspace.core + 1 );

  if (reclaimed) PANIC;

  OSTask *matched_handler = 0;

  if (queue->handlers != 0) {
    OSTask *head = queue->handlers;
    OSTask *handler = head;
    do {
      if ((!handler->handler.match_swi
         || handler->handler.swi_offset == op)
       && (!handler->handler.match_core
         || handler->handler.core == core)) {
        matched_handler = handler;
        if (queue->handlers == matched_handler)
          queue->handlers = matched_handler->next;
        if (queue->handlers == matched_handler)
          queue->handlers = 0;
        else
          dll_detach_OSTask( matched_handler );
      }
      handler = handler->next;
    } while (matched_handler == 0 && handler != head);
  }

  if (matched_handler) {
    // Schedule the handler, giving it control over caller
    // It should run on this core, so as to implement the SWI
    // functionality asap
    svc_registers *regs = &matched_handler->regs;
    regs->r[0] = ostask_handle( running );
    regs->r[1] = op;
    regs->r[2] = core;
    running->controller = matched_handler;
    dll_attach_OSTask( matched_handler, &workspace.ostask.running );
    result = matched_handler;
  }
  else {
    // No matching handler, block caller
    running->swi.offset = op;
    running->swi.core = core;
    dll_attach_OSTask( running, &queue->queue );
  }

  core_release_lock( &shared.ostask.queues_lock );

  if (result == 0) PANIC;

  return result;
}

