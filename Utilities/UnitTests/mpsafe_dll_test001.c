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
 *
 */

#include "CK_types.h"
#include "mpsafe_dll.h"

uint32_t change_word_if_equal( uint32_t *lock, uint32_t cmp, uint32_t val )
{
  uint32_t old = *lock;
  if (old == cmp) *lock = val;
  return old;
}

void push_writes_to_cache() {}
void wait_for_event() {}
void ensure_changes_observable() {}
void signal_event() {}


typedef struct example example;

struct example {
  example *prev;
  example *next;
};

MPSAFE_DLL_TYPE( example );

example items[4];

int printf( char const *fmt, ... );

int count_forwards( example *head )
{
  int result = 1;
  example *e = head->next;
  while (e != head) {
    e = e->next;
    result++;
  }
  return e == head;
}

int count_backwards( example *head )
{
  int result = 1;
  example *e = head->next;
  while (e != head) {
    e = e->prev;
    result++;
  }
  return e == head;
}

bool good_list( example *head )
{
  return head == 0 || (count_forwards( head ) == count_backwards( head ));
}

int main()
{
  for (int i = 0; i < number_of( items ); i++) {
    dll_new_example( items + i );
  }

  example *list = 0;
  example *e = 0;

  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!good_list( list )) return 1;
  if (list != &items[0]) return 1;
  if (items[0].next != &items[0]) return 1;
  if (items[0].prev != &items[0]) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!good_list( list )) return 1;
  if (list != &items[0]) return 1;
  if (items[0].next != &items[1]) return 1;
  if (items[0].prev != &items[1]) return 1;

  mpsafe_insert_example_at_head( &list, &items[2] );

  if (!good_list( list )) return 1;
  if (list != &items[2]) return 1;
  if (items[2].next != &items[0]) return 1;
  if (items[2].prev != &items[1]) return 1;

  mpsafe_insert_example_after_head( &list, &items[3] );

  if (!good_list( list )) return 1;
  if (list != &items[2]) return 1;
  if (items[2].next != &items[3]) return 1;
  if (items[2].prev != &items[1]) return 1;

  e = mpsafe_detach_example_at_head( &list );

  if (!good_list( list )) return 1;
  if (e != &items[2]) return 1;
  if (list != &items[3]) return 1;
  if (e->next != e || e->prev != e) return 1;

  e = mpsafe_detach_example_at_head( &list );

  if (!good_list( list )) return 1;
  if (e != &items[3]) return 1;
  if (list != &items[0]) return 1;
  if (e->next != e || e->prev != e) return 1;

  e = mpsafe_detach_example_at_head( &list );

  if (!good_list( list )) return 1;
  if (e != &items[0]) return 1;
  if (list != &items[1]) return 1;
  if (e->next != e || e->prev != e) return 1;

  e = mpsafe_detach_example_at_head( &list );

  if (!good_list( list )) return 1;
  if (e != &items[1]) return 1;
  if (list != 0) return 1;
  if (e->next != e || e->prev != e) return 1;

  // Starting with an empty list again...
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  // 0

  if (!good_list( list )) return 1;
  if (list != &items[0]) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  // 0 1

  if (!good_list( list )) return 1;
  if (list != &items[0]) return 1;

  mpsafe_insert_example_at_tail( &list, &items[2] );

  // 0 1 2

  if (!good_list( list )) return 1;
  if (list != &items[0]) return 1;

  mpsafe_insert_example_at_tail( &list, &items[3] );

  // 0 1 2 3

  if (!good_list( list )) return 1;
  if (list != &items[0]) return 1;

  e = mpsafe_detach_example_at_head( &list );

  // 1 2 3

  if (!good_list( list )) return 1;
  if (e != &items[0]) return 1;
  if (list != &items[1]) return 1;
  if (e->next != e || e->prev != e) return 1;

  e = mpsafe_detach_example_at_head( &list );

  // 2 3

  if (!good_list( list )) return 1;
  if (e != &items[1]) return 1;
  if (list != &items[2]) return 1;
  if (e->next != e || e->prev != e) return 1;

  e = mpsafe_detach_example_at_head( &list );

  // 3

  if (!good_list( list )) return 1;
  if (e != &items[2]) return 1;
  if (list != &items[3]) return 1;
  if (e->next != e || e->prev != e) return 1;

  e = mpsafe_detach_example_at_head( &list );

  // -

  if (!good_list( list )) return 1;
  if (e != &items[3]) return 1;
  if (list != 0) return 1;
  if (e->next != e || e->prev != e) return 1;

  return 0;
}
