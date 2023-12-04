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

// gcc -m32 sleep.c -I ../../Utilities/ -I ../.. -g
// -m32 to make pointers 32-bit, you need to install multilib, iirc

#include "CK_types.h"
#include "mpsafe_dll.h"

void _exit( int );

#define PANIC _exit( 1 )

uint32_t change_word_if_equal( uint32_t *lock, uint32_t c, uint32_t n )
{
  uint32_t v = *lock;
  if (v == c) *lock = n;
  return v;
}
static inline void ensure_changes_observable() { }

static inline void push_writes_to_cache() { }

static inline void signal_event() { }

static inline void wait_for_event() { }

int printf( char const*fmt, ... );

typedef struct OSTask OSTask;

struct OSTask {
  struct {
    uint32_t r[1];
  } regs;
  uint32_t sleep_time;
  OSTask *prev;
  OSTask *next;
};

struct {
  struct {
    OSTask *runnable;
    OSTask *sleeping;
  } ostask;
} shared = { .ostask = { 0, 0 } };


MPSAFE_DLL_TYPE( OSTask );

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
          // Is t the last entry in list?
          if (t->next == *head) break;
        }
        else {
          // t's going to be waiting longer
          t->regs.r[0] -= time;
          break;
        }
      }
      tired->regs.r[0] = time;
      // Insert before t (which may be *head)
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

int main()
{
  OSTask task[] = {
    { .sleep_time = 999 },
    { .sleep_time = 20 } };
  
  for (int i = 0; i < number_of( task ); i++) {
    OSTask *t = &task[i];
    dll_new_OSTask( t );
    t->regs.r[0] = t->sleep_time;
    sleeping_tasks_add( t );
  }

  for (int i = 0;;i++) {
    printf( "Tick %d\n", i );

    if (shared.ostask.runnable != 0) {
      OSTask *t = shared.ostask.sleeping;

      if (t != 0) do {
        printf( "%d <- %d (%d) -> %d\n", t->prev - task, t - task, t->regs.r[0], t->next - task );
        t = t->next;
      } while (t != shared.ostask.sleeping);
    }

    while (shared.ostask.runnable != 0) {
      OSTask *t = mpsafe_detach_OSTask_at_head( &shared.ostask.runnable );
      printf( "%d ", t - task );
      t->regs.r[0] = t->sleep_time;
      sleeping_tasks_add( t );
    }
    printf( "\n" );

    {
      OSTask *t = shared.ostask.sleeping;

      do {
        printf( "%d <- %d (%d) -> %d\n", t->prev - task, t - task, t->regs.r[0], t->next - task );
        t = t->next;
      } while (t != shared.ostask.sleeping);

      printf( "\n" );
    }

    sleeping_tasks_tick();
  }
  return 0;
}

