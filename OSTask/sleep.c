/* Copyright 2024 Simon Willcocks
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

static inline 
void put_to_sleep( OSTask **head, void *p )
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
          if (t == *head) break;
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
}

static inline 
OSTask *wakey_wakey( OSTask **headptr, void *p )
{
  p = p;

  OSTask *head = *headptr;
  OSTask *t = head;

  if (t == 0) return 0;
  if (0 < --t->regs.r[0]) return 0;

  OSTask *end;

  do {
    end = t;
    t = t->next;
  } while (t->regs.r[0] == 0 && t != head);

  assert( end->regs.r[0] == 0 );

  dll_detach_OSTasks_until( headptr, end );

  return head;
}

static inline 
void add_woken( OSTask **headptr, void *p )
{
  OSTask *head = *headptr;

  dll_insert_OSTask_list_at_head( p, headptr );
  // I really want it at the tail... I think.
  if (head != 0) *headptr = head;
}

void sleeping_tasks_add( OSTask *tired )
{
  mpsafe_manipulate_OSTask_list( &shared.ostask.sleeping, put_to_sleep, tired );
}

void sleeping_tasks_tick()
{
  OSTask *list = mpsafe_manipulate_OSTask_list_returning_item( &shared.ostask.sleeping, wakey_wakey, 0 );
  if (list != 0)
    mpsafe_manipulate_OSTask_list( &shared.ostask.runnable, add_woken, list );
}

