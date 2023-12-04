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

/* Lock states:
 *   Idle: 0
 *   Owned: Bits 31-1 contain task id, bit 0 set if tasks want the lock
 * Once owned, the lock value will only be changed by:
 *      a waiting task setting bit 0, or
 *      the owning task releasing the lock
 * In the latter case, the new state will either be idle, or the id of
 * a newly released waiting thread, with bit 0 set according to whether
 * there are tasks queued waiting.
 *
 * If there are none queued, but the lock is wanted by another task,
 * that task will be waiting in a call to Claim, which will set the
 * bit before the task is blocked. Either the releasing task will see
 * the set bit (and call Release) or the Claim write will fail and
 * re-try with an idle lock.
 */

typedef union {
  uint32_t raw;
  struct {
    uint32_t wanted:1;
    uint32_t half_handle:31;
  };
} OSTaskLock;

OSTask *TaskOpLockClaim( svc_registers *regs )
{
  // TODO check valid address for task (and move the task to a
  // "sin bin" - it's to big an error to ignore).
  uint32_t *lock = (void *) regs->r[0];
  regs->r[0] = 0; // Default boolean result - not already owner.

  OSTask *running = workspace.ostask.running;
  OSTask *next = running->next;

  uint32_t handle = regs->r[1];

  if (handle != ostask_handle( running )) PANIC;

  OSTaskLock code = { .raw = handle };

  OSTaskLock old;

  old.raw = change_word_if_equal( lock, 0, code.raw );

  if (old.raw == 0) {
    return 0;
  }
  else if ((~1 & old.raw) == handle) {
    regs->r[0] = 1; // boolean result - already owner (reclaimed).
    return 0;
  }

  // If we reach here, we're not the owner of the lock, the lock might
  // have its wanted bit set, or not.

  bool reclaimed = lock_ostask();
  if (reclaimed) PANIC;

  // While we were waiting for the ostasks lock, another core may have
  // updated the lock. (By setting the wanted bit or clearing the lock
  // to zero.) No other core can do that now we own the ostasks lock.

  old.raw = change_word_if_equal( lock, 0, code.raw );

  if (old.raw != 0) {
    // Going to be blocked (and not released until the ostasks lock has
    // been released.

    save_task_state( regs );
    workspace.ostask.running = next;
    dll_detach_OSTask( running );

    dll_attach_OSTask( running, &shared.ostask.blocked );
    // Put at tail so FIFO
    shared.ostask.blocked = shared.ostask.blocked->next;

    if (!old.wanted) {
      // The owner cannot have released the lock between the last read
      // and now, because LockRelease couldn't claim the ostasks lock.
      old.wanted = 1;
      *lock = old.raw;
      push_writes_to_cache();
    }
  }
  else {
    // We got the lock after all
    next = 0;
  }

  release_ostask();

  return next;
}

OSTask *TaskOpLockRelease( svc_registers *regs )
{
  // TODO check valid address for task
  uint32_t r0 = regs->r[0];
  uint32_t *lock = (void *) r0;

  OSTask *running = workspace.ostask.running;
  OSTaskSlot *slot = running->slot;

  if ((~1 & *lock) != ostask_handle( running )) PANIC; // Sin bin!

  bool reclaimed = lock_ostask();
  if (reclaimed) PANIC;

  OSTask *resume = 0;
  OSTask *t = shared.ostask.blocked;

  if (t != 0) {
    extern uint8_t app_memory_limit;

    // TODO: have slot-based blocked lists as well as the shared one,
    // makes the search time lower (although these locks shouldn't be
    // used to excess).

    bool ignore_slots = (regs->r[0] >= (uint32_t) &app_memory_limit);
    bool still_waiting = false;

    do {
      if (t->regs.r[0] == r0
       && (!ignore_slots && t->slot == slot)) {
        resume = t;
      }
      t = t->next;
    } while (resume == 0 && t != shared.ostask.blocked);

    if (resume == 0) PANIC;

    if (resume == t) { // Only item
      shared.ostask.blocked = 0;
    }
    else {
      shared.ostask.blocked = t; // = resume->next;
      dll_detach_OSTask( resume );

      do {
        still_waiting = (t->regs.r[0] == r0
                      && (!ignore_slots && t->slot == slot));
        t = t->next;
      } while (!still_waiting && t != shared.ostask.blocked);
    }

    // This was checked in LockClaim
    assert( ostask_handle( resume ) == resume->regs.r[1] );

    OSTaskLock new_owner = { .raw = resume->regs.r[1] };
    if (still_waiting) new_owner.wanted = 1;

    *lock = new_owner.raw;

    mpsafe_insert_OSTask_at_head( &shared.ostask.runnable, resume );
  }
  else {
    *lock = 0;
  }

  release_ostask();

  return 0;
}

