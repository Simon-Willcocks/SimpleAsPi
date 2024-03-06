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
#include "doubly_linked_list.h"

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

DLL_TYPE( example );

example items[8];

int printf( char const *fmt, ... );

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

bool make_ABCDEF()
{
  if (!check_list( list, "" )) return 1;

  dll_attach_example( &items[0], &list );
  if (!check_list( list, "A" )) return 1;

  dll_attach_example( &items[1], &list );
  list = list->next; // New item at tail
  if (!check_list( list, "AB" )) return 1;

  dll_attach_example( &items[2], &list );
  list = list->next; // New item at tail
  if (!check_list( list, "ABC" )) return 1;

  dll_attach_example( &items[3], &list );
  list = list->next; // New item at tail
  if (!check_list( list, "ABCD" )) return 1;

  dll_attach_example( &items[4], &list );
  list = list->next; // New item at tail
  if (!check_list( list, "ABCDE" )) return 1;

  dll_attach_example( &items[5], &list );
  list = list->next; // New item at tail
  if (!check_list( list, "ABCDEF" )) return 1;
}

// Concentrating on dll_insert_..._list_at_head
// Destination list consists of 0, 1, 2, or 3 items
// Added list consists of 1, 2, or 3 items
int main()
{
  // 3 in destination list, 3 in added list
  reset();

  if (!make_ABCDEF()) return 1;
  if (!check_list( list, "ABCDEF" )) return 1;

  list2 = list;
  dll_detach_examples_until( &list, &items[0] );

  if (!check_list( list2, "A" )) return 1;
  if (!check_list( list, "BCDEF" )) return 1;

  list2 = list;
  dll_detach_examples_until( &list, &items[4] );

  if (!check_list( list2, "BCDE" )) return 1;
  if (!check_list( list, "F" )) return 1;

  list2 = list;
  dll_detach_examples_until( &list, &items[5] );

  if (!check_list( list2, "F" )) return 1;
  if (!check_list( list, "" )) return 1;

  return 0;
}
