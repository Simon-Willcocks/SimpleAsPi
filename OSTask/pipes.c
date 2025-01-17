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

/* Improved implementation.
 * Type 1:
 *      Memory owned by pipe
 *      Whole number of pages
 *      Doubly mapped, so any access is contiguous in virtual memory
 *      Good for:
 *              File system data blocks
 *              Data streams of fixed or variable sized messages
 *
 * Type 2:
 *      Memory owned by creating slot
 *      Any size other than zero (not exceeding slot area, obvs.)
 *      Singly mapped; space/data only reported to the end of the area?
 *              (Try very hard not to overflow the end!)
 *      Slot memory will be marked as pipe locked, so it can't be reallocated
 *      Good for:
 *              areas to read files into,
 *              small buffers of bytes,
 *              not needing data at all (synchronisation)
 *
 *      TODO: Type 2 in type 1 pipes. Say, a line from a text file, maybe?
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

  uint32_t memory;      // Physical, if owner == 0, virtual otherwise
  OSTaskSlot *owner;    // NULL, if memory is owned by the pipe
  uint32_t max_block_size;
  uint32_t max_data;
  uint32_t write_index;
  uint32_t read_index;
};

MPSAFE_DLL_TYPE( OSPipe );

extern OSPipe OSPipe_free_pool[];

void setup_pipe_pool()
{
  // FIXME: free the size from the 64KiB limit
  memory_mapping pipes = {
    .base_page = claim_contiguous_memory( 0x10 ), // 64 KiB
    .pages = 0x10,
    .vap = &OSPipe_free_pool,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (pipes.base_page == 0xffffffff) PANIC;
  map_memory( &pipes );
  for (int i = 0; i < 0x10000 / sizeof( OSPipe ); i++) {
    OSPipe *p = &OSPipe_free_pool[i];
    memset( p, 0, sizeof( OSPipe ) );
    dll_new_OSPipe( p );
    dll_attach_OSPipe( p, &shared.ostask.pipe_pool );
    shared.ostask.pipe_pool = shared.ostask.pipe_pool->next;
  }

}

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
  mpsafe_insert_OSPipe_at_tail( &shared.ostask.pipe_pool, pipe );
}

DECLARE_ERROR( NotATask );
DEFINE_ERROR( NotYourPipe, 0x888, "Pipe not owned by this task" );
DEFINE_ERROR( PipeCreationError, 0x888, "Pipe creation error" );
DEFINE_ERROR( PipeCreationProblem, 0x888, "Pipe creation problem" );

OSTask *PipeCreate( svc_registers *regs )
{
  uint32_t max_block_size = regs->r[1];
  uint32_t max_data = regs->r[2];
  uint32_t allocated_mem = regs->r[3];

  OSTask *running = workspace.ostask.running;
  OSTaskSlot *slot = running->slot;

  if (max_data != 0 && max_block_size > max_data) {
    return Error_PipeCreationError( regs );
  }

  OSPipe *pipe = mpsafe_detach_OSPipe_at_head( &shared.ostask.pipe_pool );

  if (pipe == 0) {
    return Error_PipeCreationProblem( regs );
  }

  // At this point, the running task is the only one that knows about it.
  // If it goes away, the resource should be cleaned up.
  pipe->sender = pipe->receiver = running;
  pipe->sender_va = pipe->receiver_va = 0;

  pipe->max_block_size = max_block_size;
  pipe->max_data = max_data;
  pipe->memory = allocated_mem;
  if (allocated_mem == 0) {
    pipe->owner = 0;

    if (!((max_block_size & 0xfff) == 0)) PANIC; // Needs proper checking...

    pipe->memory = claim_contiguous_memory( pipe->max_block_size >> 12 );
    if (pipe->memory == 0 || pipe->memory == 0xffffffff) PANIC;
    // Needs to be in bytes for small pipes to work
    pipe->memory = pipe->memory << 12;
  }
  else {
    // FIXME Check if owner owns memory, either in slot or pipe area.
    pipe->owner = slot;
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

void create_log_pipe()
{
  // The debug pipe sender side gets mapped into core-specific memory
  // on startup. All others get mapped into slot-specific virtual
  // memory and inserted into the translation tables on data aborts.
  extern uint8_t log_pipe; // Ensure the size and the linker script match
  extern uint8_t log_pipe_top; // Ensure the size and the linker script match

  if (&log_pipe_top == &log_pipe) return; // No log pipe

  OSPipe *pipe = mpsafe_detach_OSPipe_at_head( &shared.ostask.pipe_pool );

  if (pipe == 0) PANIC;

  uint32_t size = (&log_pipe_top - &log_pipe);
  if (0 != (size & 0x1fff)) PANIC;

  pipe->sender = 0;
  pipe->receiver = 0;
  pipe->sender_va = pipe->receiver_va = 0;

  pipe->max_block_size = size / 2;
  pipe->max_data = pipe->max_block_size;
  pipe->owner = 0;
  pipe->memory = claim_contiguous_memory( pipe->max_block_size >> 12 );
  if (pipe->memory == 0 || pipe->memory == 0xffffffff) PANIC;

  pipe->memory = pipe->memory << 12;

  pipe->sender_waiting_for = 0;
  pipe->receiver_waiting_for = 0;

  pipe->write_index = 0;
  pipe->read_index = 0;

  pipe->sender_va = &log_pipe - (uint8_t*) 0;

  memory_mapping map = {
    .base_page = pipe->memory >> 12,
    .pages = pipe->max_block_size >> 12,
    .va = pipe->sender_va,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 0,
    .usr32_access = 0 };
  map_memory( &map );

  char *va = map.vap;
  va[0] = 'L';
  va[1] = 'O';
  va[2] = 'G';
  va[3] = '0' + workspace.core;
  va[4] = '\n';
  pipe->write_index = 5;

  map.va += pipe->max_block_size;
  map_memory( &map );

  dll_attach_OSPipe( pipe, &shared.ostask.pipes );

  workspace.ostask.log_pipe = pipe;
}

static uint32_t __attribute__(( noinline )) pipe_map_size( OSPipe *pipe )
{
  bool double_mapped = pipe->owner == 0;
  uint32_t map_size = pipe->max_block_size;

  if (double_mapped) {
    map_size = map_size * 2;
  }
  else {
    // Need to map whole area into pipes area
    uint32_t base_page = pipe->memory & ~0xfff;
    uint32_t above_last = (pipe->memory + pipe->max_block_size + 0xfff) & ~0xfff;
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
    if (potential_va + size >= top) PANIC;
  }

  if (block->pages != 0) PANIC;
  if (block - first >= number_of( slot->pipe_mem )) PANIC;

  bool double_mapped = pipe->owner == 0;
  if (double_mapped) {
    // Create two blocks, same physical address, consecutive
    // virtual addresses.
    if (block - first + 1 >= number_of( slot->pipe_mem )) PANIC;

    block[0].va_page = potential_va >> 12;
    block[0].pages = (size/2) >> 12;
    block[0].page_base = pipe->memory >> 12;
    block[0].device = 0;
    block[0].read_only = sender ? 0 : 1;
    block[1].va_page = (potential_va + size/2) >> 12;
    block[1].pages = (size/2) >> 12;
    block[1].page_base = pipe->memory >> 12;
    block[1].device = 0;
    block[1].read_only = sender ? 0 : 1;
  }
  else {
    block[0].va_page = potential_va >> 12;
    block[0].pages = size >> 12;
    block[0].page_base = pipe->memory >> 12;
    block[0].device = 0;
    block[0].read_only = sender ? 0 : 1;
  }

  if (sender) {
    pipe->sender_va = potential_va;
  }
  else {
    pipe->receiver_va = potential_va;
  }

#ifdef DEBUG__SHOW_PIPE_BLOCKS
  block = first;
  Task_LogString( "Pipe blocks in ", 0 );
  Task_LogHex( (uint32_t) slot );
  Task_LogNewLine();
  while (block->pages != 0
      && block - first < number_of( slot->pipe_mem )) {
    Task_LogHex( block->va_page << 12 );
    Task_LogString( " ", 1 );
    Task_LogHex( block->page_base << 12 );
    Task_LogString( " ", 1 );
    Task_LogSmallNumber( block->pages );
    Task_LogString( " ", 1 );
    Task_LogSmallNumber( block->read_only );
    Task_LogString( "\n", 1 );
    if (block->device) PANIC;
    block++;
  }
  if (sender) Task_LogString( "Set sender of ", 0 );
  else Task_LogString( "Set receiver of ", 0 );
  Task_LogHex( (uint32_t) pipe );
  Task_LogNewLine();
#endif

  return true;
}

static void set_sender_va( OSTaskSlot *slot, OSPipe *pipe )
{
  if (pipe->memory == 0) PANIC;

  if (pipe->owner != 0) {
    if (pipe->owner == slot) {
      pipe->sender_va = pipe->memory;
    }
    else PANIC;
  }

  if (workspace.ostask.log_pipe == pipe) PANIC;

  if (!insert_pipe_in_gap( slot, pipe, true )) {
    // FIXME report error!
    PANIC;
  }

#ifdef DEBUG__SHOW_PIPE_MEMORY
  Task_LogString( "Set sender VA of ", 0 );
  Task_LogHex( pipe_handle( pipe ) );
  Task_LogString( " in slot ", 0 );
  Task_LogHex( (uint32_t) slot );
  Task_LogString( " to ", 0 );
  Task_LogHex( pipe->sender_va );
  Task_LogString( " on core ", 9 );
  Task_LogSmallNumber( workspace.core );
  Task_LogNewLine();
#endif
}

static void set_receiver_va( OSTaskSlot *slot, OSPipe *pipe )
{
  if (pipe->memory == 0) PANIC;

  if (pipe->owner != 0) {
    if (pipe->owner == slot) {
      pipe->receiver_va = pipe->memory;
    }
    else PANIC; // Map the blocks containing the memory into task's pipe area
  }

  if (!insert_pipe_in_gap( slot, pipe, false )) {
    // FIXME report error!
    PANIC;
  }

#ifdef DEBUG__SHOW_PIPE_MEMORY
  Task_LogString( "Set receiver VA of ", 0 );
  Task_LogHex( pipe_handle( pipe ) );
  Task_LogString( " in slot ", 0 );
  Task_LogHex( (uint32_t) slot );
  Task_LogString( " to ", 0 );
  Task_LogHex( pipe->sender_va );
  Task_LogString( " on core ", 9 );
  Task_LogSmallNumber( workspace.core );
  Task_LogNewLine();
#endif
}

static uint32_t data_in_pipe( OSPipe *pipe )
{
  return pipe->write_index - pipe->read_index;
}

static uint32_t space_in_pipe( OSPipe *pipe )
{
  return pipe->max_block_size - data_in_pipe( pipe );
}

static uint32_t read_location( OSPipe *pipe )
{
  bool double_mapped = pipe->owner == 0;
  if (double_mapped)
    return pipe->receiver_va + (pipe->read_index % pipe->max_block_size);
  else
    return pipe->receiver_va + pipe->read_index;
}

static uint32_t write_location( OSPipe *pipe )
{
  bool double_mapped = pipe->owner == 0;
  if (double_mapped)
    return pipe->sender_va + (pipe->write_index % pipe->max_block_size);
  else
    return pipe->sender_va + pipe->write_index;
}

OSTask *PipeWaitForSpace( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];

  OSTask *running = workspace.ostask.running;
  OSTaskSlot *slot = running->slot;

  bool is_normal_pipe = (pipe != workspace.ostask.log_pipe);

  if (pipe->sender != running
   && pipe->sender != 0
   && is_normal_pipe) {
    return Error_NotYourPipe( regs );
  }

  if (is_normal_pipe && pipe->sender == 0) {
    pipe->sender = running;
  }

  if (pipe->sender_va == 0) {
    assert( pipe != workspace.ostask.log_pipe );
    set_sender_va( slot, pipe );
  }

  uint32_t available = space_in_pipe( pipe );

  if (available >= amount || pipe_receiver_finished( pipe )) {
    regs->r[1] = available;
    regs->r[2] = write_location( pipe );
  }
  else {
    pipe->sender_waiting_for = amount;

    // Blocked, waiting for space.
    return stop_running_task( regs );
  }

  return 0;
}

DEFINE_ERROR( OverfilledPipe, 0x888, "Overfilled pipe" );
DEFINE_ERROR( NotThatMuchAvailable, 0x888, "Consumed more than available" );

OSTask *PipeSpaceFilled( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];

  OSTask *running = workspace.ostask.running;

  if (pipe->sender != running
   && (pipe != workspace.ostask.log_pipe)) {
    // No setting of sender, here, if the task hasn't already checked for
    // space, how is it going to have written to the pipe?
    return Error_NotYourPipe( regs );
  }

  uint32_t available = space_in_pipe( pipe );

  if (available < amount) {
    return Error_OverfilledPipe( regs );
  }

  pipe->write_index += amount;

  // Update the caller's idea of the state of the pipe
  regs->r[1] = available - amount;
  regs->r[2] = write_location( pipe );

  OSTask *receiver = pipe->receiver;

  // If there is no receiver, there's nothing to wait for data.
  if (receiver == 0 && pipe->receiver_waiting_for != 0) PANIC;
  // If the receiver is running, it is not waiting for data.
  if ((receiver == running) && (pipe->receiver_waiting_for != 0)) PANIC;

  if (pipe->receiver_waiting_for > 0
   && pipe->receiver_waiting_for <= data_in_pipe( pipe )) {
    pipe->receiver_waiting_for = 0;

    receiver->regs.r[1] = data_in_pipe( pipe );
    receiver->regs.r[2] = read_location( pipe );

    if (workspace.ostask.running != running) PANIC;

#ifdef DEBUG__FOLLOW_TASKS_A_LOT
    Task_LogString( "< ", 2 );
    Task_LogHex( ostask_handle( running ) );
    Task_LogNewLine();
#endif
    // Make the receiver ready to run; this could be taken up instantly,
    // this core has no more control over this task.
    mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, receiver );
  }

  return 0;
}

void unmap_and_free( OSTaskSlot *slot, uint32_t va, bool double_mapped )
{
  uint32_t page = va >> 12;
  app_memory_block *first = &slot->pipe_mem[0];
  app_memory_block *block = first;
  while (block->va_page != page) {
    block++;
  }
  int remove = double_mapped ? 2 : 1;
  block--;
  do {
    block++;
    block[0] = block[remove];
  } while (block->pages != 0);
}

OSTask *PipeSetSender( svc_registers *regs, OSPipe *pipe )
{
  if (pipe->sender != 0
   && pipe->sender != workspace.ostask.running) {
    return Error_NotYourPipe( regs );
  }

  OSTask *task = ostask_from_handle( regs->r[1] );
  if (regs->r[1] != 0 && task == 0) {
    return Error_NotATask( regs );
  }

  if (pipe->sender == 0 || task == 0 || pipe->sender->slot != task->slot) {
    if (pipe->sender != 0) {
#ifdef DEBUG__SHOW_PIPE_MEMORY
  Task_LogString( "Reset sender VA of ", 0 );
  Task_LogHex( pipe_handle( pipe ) );
  Task_LogString( " in slot ", 9 );
  Task_LogHex( (uint32_t) pipe->sender->slot );
  Task_LogString( " on core ", 9 );
  Task_LogSmallNumber( workspace.core );
  Task_LogNewLine();
#endif

      // Unmap and free the virtual area for re-use
      bool double_mapped = pipe->owner == 0;
      OSTaskSlot *slot = pipe->sender->slot;
      unmap_and_free( slot, pipe->sender_va, double_mapped );
    }
    pipe->sender_va = 0;
  }

  pipe->sender = task;

  return 0;
}

OSTask *PipeUnreadData( svc_registers *regs, OSPipe *pipe )
{
  regs->r[1] = data_in_pipe( pipe );

  return 0;
}

OSTask *PipeNoMoreData( svc_registers *regs, OSPipe *pipe )
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

OSTask *PipeWaitForData( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];

  OSTask *running = workspace.ostask.running;
  OSTaskSlot *slot = running->slot;

  // log_pipe is not a special case, here; only one task can receive from it.
  if (pipe->receiver != running
   && pipe->receiver != 0) {
    return Error_NotYourPipe( regs );
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
    regs->r[2] = read_location( pipe );

    if ((regs->spsr & VF) != 0) PANIC;
  }
  else {
    pipe->receiver_waiting_for = amount;

    // Blocked, waiting for data.
    return stop_running_task( regs );
  }

  return 0;
}

OSTask *PipeDataConsumed( svc_registers *regs, OSPipe *pipe )
{
  uint32_t amount = regs->r[1];

  OSTask *running = workspace.ostask.running;

  if (pipe->receiver != running) {
    // No setting of receiver, here, if the task hasn't already checked for
    // data, how is it going to have read from the pipe?
    return Error_NotYourPipe( regs );
  }

  uint32_t available = data_in_pipe( pipe );

  if (available < amount) {
    return Error_NotThatMuchAvailable( regs );
  }

  pipe->read_index += amount;

  // Update the caller's idea of the state of the pipe
  regs->r[1] = available - amount;
  regs->r[2] = read_location( pipe );

  if (pipe->sender_waiting_for > 0
   && pipe->sender_waiting_for <= space_in_pipe( pipe )) {
    // Space is now available
    OSTask *sender = pipe->sender;

    pipe->sender_waiting_for = 0;

    sender->regs.r[1] = space_in_pipe( pipe );
    sender->regs.r[2] = write_location( pipe );

#ifdef DEBUG__FOLLOW_TASKS_A_LOT
    Task_LogString( "> ", 2 );
    Task_LogHex( ostask_handle( running ) );
    Task_LogNewLine();
#endif
    // Make the sender ready to run; this could be taken up instantly,
    // this core has no more control over this task.
    mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, sender );
  }

  return 0;
}

OSTask *PipeSetReceiver( svc_registers *regs, OSPipe *pipe )
{
  if (pipe->receiver != 0
   && pipe->receiver != workspace.ostask.running) {
    return Error_NotYourPipe( regs );
  }

  OSTask *task = ostask_from_handle( regs->r[1] );
  if (regs->r[1] != 0 && task == 0) {
    return Error_NotATask( regs );
  }

  if (pipe->receiver == 0 || task == 0 || pipe->receiver->slot != task->slot) {
    pipe->receiver_va = 0;
    if (pipe->receiver != 0) {
#ifdef DEBUG__SHOW_PIPE_MEMORY
  Task_LogString( "Reset sender VA of ", 0 );
  Task_LogHex( pipe_handle( pipe ) );
  Task_LogString( " in slot ", 0 );
  Task_LogHex( (uint32_t) pipe->receiver->slot );
  Task_LogString( " on core ", 9 );
  Task_LogSmallNumber( workspace.core );
  Task_LogNewLine();
#endif

      // Unmap and free the virtual area for re-use
      bool double_mapped = pipe->owner == 0;
      OSTaskSlot *slot = pipe->receiver->slot;
      unmap_and_free( slot, pipe->receiver_va, double_mapped );
    }
  }

  pipe->receiver = task;

  return 0;
}

OSTask *PipeNotListening( svc_registers *regs, OSPipe *pipe )
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

OSTask *TaskOpGetLogPipe( svc_registers *regs )
{
  if (0 == workspace.ostask.log_pipe) {
    regs->r[0] = 0;
    return 0;
  }

  if (workspace.ostask.log_pipe->receiver != 0) {
    // TODO Inform the previous receiver before resuming this new owner
    PANIC;
  }

  regs->r[0] = pipe_handle( workspace.ostask.log_pipe );
  workspace.ostask.log_pipe->receiver = 0;

  return 0;
}

OSTask *TaskOpLogString( svc_registers *regs )
{
  char const *string = (void*) regs->r[0];
  uint32_t length = regs->r[1];

#ifdef DEBUG__SEQUENCE_LOG_ENTRIES
  uint32_t index;
  if (!workspace.ostask.no_index) {
    uint32_t *p = &shared.ostask.log_index;
    do {
      index = *p;
    } while (index != change_word_if_equal( p, index, index + 1 ));
    length += 5; // Extra space
  }
#endif

  OSPipe *pipe = workspace.ostask.log_pipe;

  // Never blocks, that could lock the kernel!
  if (pipe == 0) return 0;

  uint32_t available = space_in_pipe( pipe );
  if (available < length) return 0;

  char *dest = (void*) write_location( pipe );

#ifdef DEBUG__SEQUENCE_LOG_ENTRIES
  if (!workspace.ostask.no_index) {
    // Octal!
    dest[0] = '0' + (7 & (index >> 9));
    dest[1] = '0' + (7 & (index >> 6));
    dest[2] = '0' + (7 & (index >> 3));
    dest[3] = '0' + (7 & (index >> 0));
    dest[4] = ' ';
    length -= 5;
    dest += 5;
  }
#endif

  for (int i = 0; i < length; i++) {
    dest[i] = string[i];
  }

#ifdef DEBUG__SEQUENCE_LOG_ENTRIES
  if (!workspace.ostask.no_index)
    length += 5;
  workspace.ostask.no_index = string[length-1] != '\n';
#endif

  // PipeSpaceFilled only looks at the given length, but fills in
  // the caller's idea of the state of the pipe, which we're not
  // interested in.
  svc_registers tmp;    // Note: don't initialise here, or memset gets called
  tmp.r[1] = length;
  OSTask *resume = PipeSpaceFilled( &tmp, pipe );

  // It also never changes the running task
  assert( resume == 0 );

  return resume;
}
