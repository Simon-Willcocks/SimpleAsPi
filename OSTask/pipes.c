/* Copyright 2021-2023 Simon Willcocks
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

typedef struct OSPipe OSPipe;

/* Initial implementation of pipes:
 *  4KiB each
 *  Located at top of bottom MiB (really needs fixing next!)
 *  debug pipe a special case, mapped in top MiB
 */

struct OSPipe {
  OSPipe *next;
  OSPipe *prev;
  OSTask *sender;
  uint32_t sender_waiting_for; // Non-zero if blocked
  uint32_t sender_va; // Zero if not allocated
  OSTask *receiver;
  uint32_t receiver_waiting_for; // Non-zero if blocked
  uint32_t receiver_va; // Zero if not allocated

  uint32_t physical;
  uint32_t allocated_mem;
  uint32_t max_block_size;
  uint32_t max_data;
  uint32_t write_index;
  uint32_t read_index;
};

DLL_TYPE( OSPipe );

static inline void mark_pipe_sender_finished( OSPipe *pipe )
{
  pipe->sender = (void*) -1;
}

static inline void mark_pipe_receiver_finished( OSPipe *pipe )
{
  pipe->receiver = (void*) -1;
}

static inline bool pipe_sender_finished( OSPipe *pipe )
{
  return pipe->sender == (void*) -1;
}

static inline bool pipe_receiver_finished( OSPipe *pipe )
{
  return pipe->receiver == (void*) -1;
}

static inline void free_pipe( OSPipe* pipe )
{
  if (shared.ostask.pipes == pipe) shared.ostask.pipes = pipe->next;
  if (shared.ostask.pipes == pipe) shared.ostask.pipes = 0;
  if (shared.ostask.pipes == pipe) PANIC;

  dll_detach_OSPipe( pipe );

  system_heap_free( pipe );
}

bool this_is_debug_receiver()
{
  OSTask *running = workspace.ostask.running;
  OSPipe *pipe = workspace.ostask.debug_pipe;
  return running == pipe->receiver;
}

static inline
bool in_range( uint32_t value, uint32_t base, uint32_t size )
{
  return (value >= base && value < (base + size));
}

// The debug pipe sender side gets mapped into core-specific memory
// on startup. All others get mapped into slot-specific virtual
// memory and inserted into the translation tables on data aborts.
void *set_and_map_debug_pipe()
{
  extern uint32_t debug_pipe; // Ensure the size and the linker script match
  OSPipe *pipe = workspace.ostask.debug_pipe;
  uint32_t va = (uint32_t) &debug_pipe;

  if (pipe->sender_va == 0) {
    // This is the core's debug pipe, and it hasn't been mapped yet.
    pipe->sender_va = va;

    memory_mapping map = {
      .base_page = pipe->physical >> 12,
      .pages = pipe->max_block_size >> 12,
      .va = va,
      .type = CK_MemoryRW,
      .map_specific = 0,
      .all_cores = 0,
      .usr32_access = 0 };
    map_memory( &map );
    map.va += pipe->max_block_size;
    map_memory( &map );
  }

  if (pipe->sender_va != va) PANIC;
  if (pipe->sender_va == 0) PANIC;

  return (void*) va;
}

error_block *PipeOp_NotYourPipeError()
{
    PANIC;
  static error_block error = { 0x888, "Pipe not owned by this task" };
  return &error;
}

error_block *PipeOp_CreationError()
{
  static error_block error = { 0x888, "Pipe creation error" };
  return &error;
}

error_block *PipeOp_CreationProblem()
{
  static error_block error = { 0x888, "Pipe creation problem" };
  return &error;
}

error_block *PipeCreate( svc_registers *regs )
{
  uint32_t max_block_size = regs->r[1];
  uint32_t max_data = regs->r[2];
  uint32_t allocated_mem = regs->r[3];

  OSTask *running = workspace.ostask.running;

  if (max_data != 0 && max_block_size > max_data) {
    return PipeOp_CreationError();
  }

  OSPipe *pipe = system_heap_allocate( sizeof( OSPipe ) );

  if (pipe == 0) {
    return PipeOp_CreationProblem();
  }

  dll_new_OSPipe( pipe );

  // At this point, the running task is the only one that knows about it.
  // If it goes away, the resource should be cleaned up.
  pipe->sender = pipe->receiver = running;
  pipe->sender_va = pipe->receiver_va = 0;

  pipe->max_block_size = max_block_size;
  pipe->max_data = max_data;
  pipe->allocated_mem = allocated_mem;
  if (allocated_mem == 0) {
    if (!((max_block_size & 0xfff) == 0)) PANIC; // Needs proper checking...

    pipe->physical = claim_contiguous_memory( pipe->max_block_size >> 12 );
    if (pipe->physical == 0 || pipe->physical == 0xffffffff) PANIC;
    // Needs to be in bytes for small pipes to work
    pipe->physical = pipe->physical << 12;
  }
  else {
    PANIC;
#if 0
    physical_memory_block block = Kernel_physical_address( allocated_mem, running->slot );

    if (memory_block_size( block ) == 0) {
      PANIC;
    }
    // FIXME: this should be an error to return
    if (!(memory_block_size( block ) != 0)) PANIC;

    uint32_t offset = allocated_mem - memory_block_virtual_base( block );
    if (!(allocated_mem >= memory_block_virtual_base( block ))) PANIC; // Otherwise the memory is not in that block!
    pipe->physical = offset + memory_block_physical_base( block );
    if (offset + max_block_size > memory_block_size( block )) {
      // FIXME: This is a memory leak of the pipe in the RMA
      PANIC;
      return PipeOp_CreationError();
    }
#endif
  }

  // The following will be updated on the first blocking calls
  // to WaitForSpace and WaitForData, respectively.
  pipe->sender_waiting_for = 0;
  pipe->receiver_waiting_for = 0;

  pipe->write_index = 0; // allocated_mem & 0xfff;
  pipe->read_index = 0; // allocated_mem & 0xfff;

  dll_attach_OSPipe( pipe, &shared.ostask.pipes );

  regs->r[0] = pipe_handle( pipe );

  return 0;
}

static uint32_t __attribute__(( noinline )) pipe_map_size( OSPipe *pipe )
{
  bool double_mapped = pipe->allocated_mem == 0;
  uint32_t map_size = pipe->max_block_size;

  if (double_mapped) {
    map_size = map_size * 2;
  }
  else {
    // Need to map whole area into pipes area
    uint32_t base_page = pipe->allocated_mem & ~0xfff;
    uint32_t above_last = (pipe->allocated_mem + pipe->max_block_size + 0xfff) & ~0xfff;
    map_size = above_last - base_page;
  }

  return map_size;
}

static inline uint32_t top_of( app_memory_block *block )
{
  return (block->va_page + block->pages) << 12;
}

bool insert_pipe_in_gap( OSTaskSlot *slot, OSPipe *pipe, bool sender )
{
  extern uint8_t pipes_top;
  extern uint8_t pipes_base;

  uint32_t top = (&pipes_top - (uint8_t*) 0);
  uint32_t bottom = (&pipes_base - (uint8_t*) 0);

  uint32_t size = pipe_map_size( pipe );

  app_memory_block *first = &slot->pipe_mem[0];
  app_memory_block *block = first;
  uint32_t potential_va = bottom;

  // FIXME: doesn't stop at the end of the array, doesn't
  // keep in ascending order (or does it?)
  // Just really lazy.
  // FIXME: also need something to align multi-MiB pages...
  while (block->pages != 0
      && block - first < number_of( slot->pipe_mem )) {
    potential_va = top_of( block );
    block++;
  }

  if (block->pages != 0) PANIC;
  if (block - first >= number_of( slot->pipe_mem )) PANIC;

  bool double_mapped = pipe->allocated_mem == 0;
  if (double_mapped) {
    // Create two blocks, same physical address, consecutive
    // virtual addresses.
    if (block - first + 1 >= number_of( slot->pipe_mem )) PANIC;

    block[0].va_page = potential_va >> 12;
    block[0].pages = size >> 13;
    block[0].page_base = pipe->physical >> 12;
    block[0].device = 0;
    block[0].read_only = sender ? 0 : 1;
    block[1].va_page = (potential_va + size/2) >> 12;
    block[1].pages = size >> 13;
    block[1].page_base = pipe->physical >> 12;
    block[1].device = 0;
    block[1].read_only = sender ? 0 : 1;
  }
  else {
    block[0].va_page = potential_va >> 12;
    block[0].pages = size >> 12;
    block[0].page_base = pipe->physical >> 12;
    block[0].device = 0;
    block[0].read_only = sender ? 0 : 1;
  }

  if (sender) {
    *((uint32_t*) potential_va) = 0x74747474;

    pipe->sender_va = potential_va;
  }
  else {
    asm ( "" : : "r" (*((uint32_t*) potential_va)) );

    pipe->receiver_va = potential_va;
  }

  return true;
}

static void set_sender_va( OSTaskSlot *slot, OSPipe *pipe )
{
  if (pipe->physical == 0) PANIC;

  if (workspace.ostask.debug_pipe == pipe) PANIC;

  if (!insert_pipe_in_gap( slot, pipe, true )) {
    // FIXME report error!
    PANIC;
  }
}

static void set_receiver_va( OSTaskSlot *slot, OSPipe *pipe )
{
  if (pipe->physical == 0) PANIC;

  if (!insert_pipe_in_gap( slot, pipe, false )) {
    // FIXME report error!
    PANIC;
  }
}

static uint32_t data_in_pipe( OSPipe *pipe )
{
  return pipe->write_index - pipe->read_index;
}

static uint32_t space_in_pipe( OSPipe *pipe )
{
  return pipe->max_block_size - data_in_pipe( pipe );
}

static uint32_t read_location( OSPipe *pipe, OSTaskSlot *slot )
{
  bool double_mapped = pipe->allocated_mem == 0;
  if (double_mapped)
    return pipe->receiver_va + (pipe->read_index % pipe->max_block_size);
  else
    return pipe->receiver_va + pipe->read_index;
}

static uint32_t write_location( OSPipe *pipe, OSTaskSlot *slot )
{
  bool double_mapped = pipe->allocated_mem == 0;
  if (double_mapped)
    return pipe->sender_va + (pipe->write_index % pipe->max_block_size);
  else
    return pipe->sender_va + pipe->write_index;
}

error_block *PipeWaitForSpace( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];
  // TODO validation

  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;
  OSTaskSlot *slot = running->slot;

  bool is_normal_pipe = (pipe != workspace.ostask.debug_pipe);

  if (pipe->sender != running
   && pipe->sender != 0
   && is_normal_pipe) {
    return PipeOp_NotYourPipeError();
  }

  if (is_normal_pipe && pipe->sender == 0) {
    pipe->sender = running;
  }

  if (pipe->sender_va == 0) {
    if (pipe == workspace.ostask.debug_pipe)
      set_and_map_debug_pipe();
    else
      set_sender_va( slot, pipe );
  }

  uint32_t available = space_in_pipe( pipe );

  if (available >= amount || pipe_receiver_finished( pipe )) {
    regs->r[1] = available;
    regs->r[2] = write_location( pipe, slot );
  }
  else {
    pipe->sender_waiting_for = amount;

    if (running == next) PANIC;

    save_task_state( regs );
    workspace.ostask.running = next;
    regs->r[1] = 0xb00b00b0;

    // Blocked, waiting for data.
    dll_detach_OSTask( running );
  }

  return 0;
}

error_block *PipeSpaceFilled( svc_registers *regs, OSPipe *pipe )
{
  error_block *error = 0;

  uint32_t amount = regs->r[1];
  // TODO validation

  OSTask *running = workspace.ostask.running;
  OSTaskSlot *slot = running->slot;

  if (pipe->sender != running
   && (pipe != workspace.ostask.debug_pipe)) {
    // No setting of sender, here, if the task hasn't already checked for
    // space, how is it going to have written to the pipe?
    return PipeOp_NotYourPipeError();
  }

  uint32_t available = space_in_pipe( pipe );

  if (available < amount) {
    static error_block err = { 0x888, "Overfilled pipe" };
    error = &err;
  }
  else {
    pipe->write_index += amount;

    // Update the caller's idea of the state of the pipe
    regs->r[1] = available - amount;
    regs->r[2] = write_location( pipe, slot );

    OSTask *receiver = pipe->receiver;

    // If there is no receiver, there's nothing to wait for data.
    if (receiver == 0 && pipe->receiver_waiting_for != 0) PANIC;
    // If the receiver is running, it is not waiting for data.
    if ((receiver == running) && (pipe->receiver_waiting_for != 0)) PANIC;

    // Special case: the debug_pipe is sometimes filled from the reader task
    // FIXME Is it really, any more? I don't think so.

    if (pipe->receiver_waiting_for > 0
     && pipe->receiver_waiting_for <= data_in_pipe( pipe )) {
      pipe->receiver_waiting_for = 0;

      receiver->regs.r[1] = data_in_pipe( pipe );
      receiver->regs.r[2] = read_location( pipe, slot );

      if (workspace.ostask.running != running) PANIC;

      // Make the receiver ready to run when the sender blocks.
      // This could be take up instantly, this core has no more
      // control over this task.
      mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, receiver );
    }
  }

  return error;
}

error_block *PipeSetSender( svc_registers *regs, OSPipe *pipe )
{
  if (pipe->sender != workspace.ostask.running) {
    return PipeOp_NotYourPipeError();
  }
  pipe->sender = ostask_from_handle( regs->r[1] );
  pipe->sender_va = 0; // FIXME unmap and free the virtual area for re-use

  // TODO Unmap from virtual memory, if new sender is in different slot

  return 0;
}

error_block *PipeUnreadData( svc_registers *regs, OSPipe *pipe )
{
  regs->r[1] = data_in_pipe( pipe );

  return 0;
}

error_block *PipeNoMoreData( svc_registers *regs, OSPipe *pipe )
{
  // Mark the pipe as uninteresting from the sender's end
  // If it's also uninteresting from the receiver's end, delete it
  mark_pipe_sender_finished( pipe );
  if (pipe_receiver_finished( pipe )) {
    free_pipe( pipe );
  }
  else {
    // TODO
    // If the receiver is waiting for data, wake it up, even if it's waiting
    // for more data than available.
    if (pipe->receiver_waiting_for != 0) PANIC;
  }
  return 0; 
}

error_block *PipeWaitForData( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];
  // TODO validation

  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;
  OSTaskSlot *slot = running->slot;

  // debug_pipe is not a special case, here; only one task can receive from it.
  if (pipe->receiver != running
   && pipe->receiver != 0) {
    return PipeOp_NotYourPipeError();
  }

  if (pipe->receiver == 0) {
    pipe->receiver = running;
  }

  if (pipe->receiver != running) PANIC;

  if (pipe->receiver_va == 0) {
    if (pipe->max_block_size != 0 || pipe->max_data != 0)
      set_receiver_va( slot, pipe );
  }
  if (pipe->receiver_va == 0) PANIC;

  uint32_t available = data_in_pipe( pipe );

  if (available >= amount || pipe_sender_finished( pipe )) {
    regs->r[1] = available;
    regs->r[2] = read_location( pipe, slot );

    if ((regs->spsr & VF) != 0) PANIC;
  }
  else {
    pipe->receiver_waiting_for = amount;

    save_task_state( regs );
    workspace.ostask.running = next;

    if (workspace.ostask.running == running) PANIC;

    // Blocked, waiting for data.
    dll_detach_OSTask( running );
  }

  return 0;
}

error_block *PipeDataConsumed( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];
  // TODO validation

  OSTask *running = workspace.ostask.running;
  OSTaskSlot *slot = running->slot;

  if (pipe->receiver != running) {
    // No setting of receiver, here, if the task hasn't already checked for
    // data, how is it going to have read from the pipe?
    return PipeOp_NotYourPipeError();
  }

  uint32_t available = data_in_pipe( pipe );

  if (available >= amount) {
    pipe->read_index += amount;

    regs->r[1] = available - amount;
    regs->r[2] = read_location( pipe, slot );

    if (pipe->sender_waiting_for > 0
     && pipe->sender_waiting_for <= space_in_pipe( pipe )) {
      OSTask *sender = pipe->sender;

      pipe->sender_waiting_for = 0;

      sender->regs.r[1] = space_in_pipe( pipe );
      sender->regs.r[2] = write_location( pipe, slot );

      // "Returns" from SWI next time scheduled
      if (sender != running) {
        OSTask *tail = running->next;
        dll_attach_OSTask( sender, &tail );
      }
    }
  }
  else {
    PANIC; // Consumed more than available?
  }

  return 0;
}

error_block *PipeSetReceiver( svc_registers *regs, OSPipe *pipe )
{
  if (pipe->receiver != workspace.ostask.running) {
    return PipeOp_NotYourPipeError();
  }

  pipe->receiver = ostask_from_handle( regs->r[1] );
  pipe->receiver_va = 0;

  // TODO Unmap from virtual memory, if new receiver in different slot

  return 0;
}

error_block *PipeNotListening( svc_registers *regs, OSPipe *pipe )
{
  // This should mark the pipe as uninteresting from the receiver's end
  // If it's also uninteresting from the sender's end, delete it
  mark_pipe_receiver_finished( pipe );
  if (pipe_sender_finished( pipe )) {
    free_pipe( pipe );
  }
  else {
    // TODO
    // If the receiver is waiting for data, wake it up, even if it's waiting
    // for more data than available.
    if (pipe->sender_waiting_for != 0) PANIC;
  }

  return 0; 
}

#if 0
// The debug handler pipe is the special case, where every task
// can send to it, and the receiver is scheduled whenever there's
// text in the buffer and this routine is called.

// (The receiver end does not have to be special, FIXME)

// Looking from the outside!
#include "pipeop.h"

void kick_debug_handler_thread( svc_registers *regs )
{
  // Push any debug text written in SVC mode to the pipe.
  // No need to lock the pipes in this routine since:
  //   The debug pipe, if it exists, exists forever
  //   The debug pipe is associated with just one core
  //   The core is running with interrupts disabled.

  if (!((regs->spsr & 0x8f) == 0)) PANIC;
  if (!((regs->spsr & 0x80) == 0)) PANIC;

  uint32_t written = workspace.ostask.debug_written;

  if (written == 0) return;

  OSPipe *pipe = workspace.ostask.debug_pipe;

  if (pipe == 0) return;

  OSPipe *p = workspace.ostask.debug_pipe;
  OSTask *receiver = p->receiver;
  OSTask *running = workspace.ostask.running;

  if (receiver == 0 || running == receiver) {
    // Receiver is current task
  }
  else if (p->receiver_waiting_for == 0) {
    // Receiver is running
    // Make it the current task
    // dll_detach_OSTask( receiver );
    // dll_attach_OSTask( receiver, &workspace.ostask.running );
  }
  else {
    workspace.ostask.debug_written = 0;
    workspace.ostask.debug_space = PipeOp_SpaceFilled( pipe, written );

    if (workspace.ostask.running->prev == receiver) {
      // Rather than wait for the debug pipe to fill up, we'll yield to
      // the receiver.
      if (!(p->receiver_waiting_for == 0)) PANIC; // Woken by above SpaceFilled

      // About to swap out this, the sender, task
      // (Not needed when the pipe is not the debug pipe.)
      save_task_state( regs );

      workspace.ostask.running = receiver;
    }
  }
}
#endif

