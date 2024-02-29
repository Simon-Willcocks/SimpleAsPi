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

// gcc -m32 *.c -g  -I .
// -m32 to make pointers 32-bit, you need to install multilib, iirc

#include "ostask.h"

struct shared shared = { .ostask = { 0, 0 } };

int main()
{
  OSTask task[] = {
    { .sleep_time = 999 },
    { .sleep_time = 17 },
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

    if (shared.ostask.runnable != 0) printf( "\n" );

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

