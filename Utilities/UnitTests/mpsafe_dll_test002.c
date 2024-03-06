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

int strcmp(const char *s1, const char *s2)
{
  int result = 0;
  while (result == 0 && *s1 != '\0') {
    result = *s1++ - *s2++;
  }
  return result;
}

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

example items[8];

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

bool check_list( example *head, char const *expected )
{
  printf( "%s?\n", expected );
  char const *p = expected;
  bool result;
  example *i = head;
  if (i != 0) {
    do {
      printf( "%c", 'A' + (i - items) );
      if ((*expected++ - 'A') != (i - items)) break;
      i = i->next;
    } while (i != head);
  }
  printf( "\n" );
  return *expected == '\0';
}

example *list = 0;
example *list2 = 0;

void reset()
{
  for (int i = 0; i < number_of( items ); i++) {
    dll_new_example( items + i );
  }
  list = 0;
  list2 = 0;
}

// Concentrating on dll_insert_..._list_at_head
// Destination list consists of 0, 1, 2, or 3 items
// Added list consists of 1, 2, or 3 items
int main()
{
  // 3 in destination list, 3 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!check_list( list, "AB" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[2] );

  if (!check_list( list, "ABC" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[6] );

  if (!check_list( list2, "EFG" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFGABC" )) return 1;
  if (!good_list( list )) return 1;

  // 2 in destination list, 3 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!check_list( list, "AB" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[6] );

  if (!check_list( list2, "EFG" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFGAB" )) return 1;
  if (!good_list( list )) return 1;

  // 1 in destination list, 3 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[6] );

  if (!check_list( list2, "EFG" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFGA" )) return 1;
  if (!good_list( list )) return 1;

  // 0 in destination list, 3 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[6] );

  if (!check_list( list2, "EFG" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFG" )) return 1;
  if (!good_list( list )) return 1;

  // 3 in destination list, 2 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!check_list( list, "AB" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[2] );

  if (!check_list( list, "ABC" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFABC" )) return 1;
  if (!good_list( list )) return 1;

  // 2 in destination list, 2 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!check_list( list, "AB" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFAB" )) return 1;
  if (!good_list( list )) return 1;

  // 1 in destination list, 2 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EFA" )) return 1;
  if (!good_list( list )) return 1;

  // 0 in destination list, 2 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  mpsafe_insert_example_at_tail( &list2, &items[5] );

  if (!check_list( list2, "EF" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EF" )) return 1;
  if (!good_list( list )) return 1;

  // 3 in destination list, 1 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!check_list( list, "AB" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[2] );

  if (!check_list( list, "ABC" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EABC" )) return 1;
  if (!good_list( list )) return 1;

  // 2 in destination list, 1 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[1] );

  if (!check_list( list, "AB" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EAB" )) return 1;
  if (!good_list( list )) return 1;

  // 1 in destination list, 1 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  mpsafe_insert_example_at_tail( &list, &items[0] );

  if (!check_list( list, "A" )) return 1;
  if (!good_list( list )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "EA" )) return 1;
  if (!good_list( list )) return 1;

  // 0 in destination list, 1 in added list
  reset();

  if (!good_list( list )) return 1;

  if (!check_list( list, "" )) return 1;

  if (!check_list( list2, "" )) return 1;
  mpsafe_insert_example_at_tail( &list2, &items[4] );

  if (!check_list( list2, "E" )) return 1;
  if (!good_list( list2 )) return 1;

  dll_insert_example_list_at_head( list2, &list );

  if (!check_list( list, "E" )) return 1;
  if (!good_list( list )) return 1;

  return 0;
}
