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
#include "mpsafe_dll.h"

MPSAFE_DLL_TYPE( OSTask );

struct __attribute__(( packed, aligned( 4 ) )) OSQueue {
  OSTask *queue;
  OSTask *handlers;
};

static inline error_block *QueueOp_CreationProblem( svc_registers *regs )
{
  static error_block error = { 0x888, "OSTask Queue creation problem" };
  return &error;
}

uint32_t new_queue()
{
  OSQueue *queue = system_heap_allocate( sizeof( OSQueue ) );
  
  if (queue == 0) {
    return 0;
  }

  queue->handlers = 0;
  queue->queue = 0;

  return handle_from_queue( queue );
}

error_block *QueueCreate( svc_registers *regs )
{
  uint32_t handle = new_queue();
  OSQueue *queue = queue_from_handle( handle );
  
  if (queue == 0) {
    return QueueOp_CreationProblem( regs );
  }

  regs->r[0] = handle;

  return 0;
}

error_block *QueueWait( svc_registers *regs, OSQueue *queue, bool swi, bool core )
{
  // Wait for any queued OSTask

  // TODO validation, scan
  if (swi || core) PANIC;

  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;

  bool reclaimed = core_claim_lock( &shared.ostask.queues_lock,
                                    workspace.core + 1 );

  if (reclaimed) PANIC;

  if (queue->queue == 0) {
    // Nothing to remove, block caller
    save_task_state( regs );
    workspace.ostask.running = next;
    dll_detach_OSTask( running );
    dll_attach_OSTask( running, &queue->handlers );
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
    regs->r[0] = handle_from_task( head );
    regs->r[1] = head->swi.offset;
    regs->r[2] = head->swi.core;
    head->controller = running;
  }

  core_release_lock( &shared.ostask.queues_lock );

  return 0;
}

error_block *queue_running_OSTask( svc_registers *regs, uint32_t queue_handle, uint32_t SWI )
{
  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;

  OSQueue *queue = queue_from_handle( queue_handle );

  if (queue == 0) {
    return Error_InvalidQueue();
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
    regs->r[0] = handle_from_task( running );
    regs->r[1] = op;
    regs->r[2] = core;
    running->controller = matched_handler;
    dll_attach_OSTask( matched_handler, &workspace.ostask.running );
  }
  else {
    // No matching handler, block caller
    running->swi.offset = op;
    running->swi.core = core;
    dll_attach_OSTask( running, &queue->queue );
  }

  core_release_lock( &shared.ostask.queues_lock );

  return 0;
}

