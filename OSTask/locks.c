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
  OSTask *running = workspace.ostask.running;
  OSTask *next = 0;

  uint32_t handle = regs->r[1];

  if (handle != ostask_handle( running )) PANIC;

  OSTaskLock code = { .raw = handle };

  uint32_t *lock = (void *) regs->r[0];

  bool reclaimed = lock_ostask();
  if (reclaimed) PANIC;

  OSTaskLock old;
  old.raw = *lock;

  if (old.raw == 0) {
    *lock = code.raw;
    regs->r[0] = 0;
    push_writes_to_cache();
  }
  else if (old.half_handle == code.half_handle) {
    // Already owner
    regs->r[0] = 1;
  }
  else {
    next = running->next;

    save_task_state( regs );
    workspace.ostask.running = next;
    dll_detach_OSTask( running );

    assert( running->regs.r[0] == (uint32_t) lock );

    dll_attach_OSTask( running, &shared.ostask.blocked );
    // Put at tail so FIFO
    shared.ostask.blocked = shared.ostask.blocked->next;

    if (!old.wanted) {
      old.wanted = 1;
      *lock = old.raw;
    }

    push_writes_to_cache();
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

  if ((~1 & *lock) != ostask_handle( running )) PANIC; // Sin bin!

  bool reclaimed = lock_ostask();
  assert( !reclaimed );

  OSTask *resume = 0;
  OSTask *t = shared.ostask.blocked;
  if (t == 0) {
    *lock = 0;
  }
  else {
    while (t->regs.r[0] != r0 && t != shared.ostask.blocked) {
      t = t->next;
    }

    if (t->regs.r[0] == r0) {
      resume = t;

      t = resume->next;

      // Move resume away the the head of the list, if necessary
      if (resume == shared.ostask.blocked) {
        if (t == shared.ostask.blocked) {
          shared.ostask.blocked = 0;
          t = 0;
        }
        else {
          shared.ostask.blocked = t;
        }
      }

      dll_detach_OSTask( resume );

      OSTaskLock new_owner = { .raw = resume->regs.r[1] };

      if (t != 0) {
        do {
          if (t->regs.r[0] == r0) new_owner.wanted = 1;
          t = t->next;
        } while (t != shared.ostask.blocked && new_owner.wanted == 0);
      }

      *lock = new_owner.raw;

      resume->regs.r[0] = 0; // boolean result - not already owner.
      push_writes_to_cache();

      mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, resume );
    }
  }

  release_ostask();

  // Always continues the releasing task
  // Another core may already have taken up the new owner, if any.
  // This task will have to wait for the new owner if it tries to
  // claim the lock again.

  return 0;
}
