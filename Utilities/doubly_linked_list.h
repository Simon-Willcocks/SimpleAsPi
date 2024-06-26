/* Copyright 2022 Simon Willcocks
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

// For any struct with next and prev fields that are pointers to that type
// of structure.

#ifdef DLL_VERIFICATION
#define dll_assert( x ) assert( x )
#else
#define dll_assert( x )
#endif

#define DLL_TYPE( T ) \
/* Initialise the item as a list of one item. */ \
static inline void dll_new_##T( T *i ) { \
  i->next = i; \
  i->prev = i; \
} \
 \
/* Attach the item as the head of the list. (If you want it at the tail, */ \
/* follow up with `l = l->next;`, if you want it after the head, declare */ \
/* a list `T *tmp = l->next;`, then attach it to that list. (Remember to */ \
/* check for an empty list in that case!) */ \
static inline void dll_attach_##T( T *i, T * volatile*l ) { \
  dll_assert( i->next == i && i->prev == i ); \
  dll_assert( i != 0 ); \
  T *head = *l; \
  if (head != 0) { \
    i->next = head; \
    i->prev = head->prev; \
    i->prev->next = i; \
    head->prev = i; \
  } \
  (*l) = i; \
} \
 \
/* Detatch the item from any list it is in (if it is the head of a */ \
/* list, it will effectively detach the rest of the list instead!) */ \
static inline void dll_detach_##T( T *i ) { \
  dll_assert( i->prev->next == i ); \
  dll_assert( i->next->prev == i ); \
  i->prev->next = i->next; \
  i->next->prev = i->prev; \
  i->next = i; \
  i->prev = i; \
  dll_assert( i->next == i && i->prev == i ); \
} \
 \
/* Move the item from list 1 to list 2 (which should be pointers \
   to the head of the list). */ \
static inline void dll_move_##T( T *i, T * volatile*l1, T * volatile*l2 ) { \
  if ((*l1) == i) { \
    (*l1) = i->next; \
    if ((*l1) == i) { \
      (*l1) = 0; \
    } \
  } \
  if ((*l2) == 0) (*l2) = i; \
  i->prev->next = i->next; \
  i->next->prev = i->prev; \
  i->next = (*l2); \
  i->prev = (*l2)->prev; \
  (*l2)->prev = i; \
  (*l2)->prev->next = i; \
} \
 \
/* Replace item 1 with item 2 in whatever list it may be in. */ \
/* If item 1 was the head of the list, the head of the list will be item 2 */\
static inline void dll_replace_##T( T *i1, T *i2, T * volatile*l ) { \
  dll_assert( i1 != i2 ); \
  dll_assert( i2->next == i2 ); \
  dll_assert( i2->prev == i2 ); \
  if (i1->next == i1) { \
    /* Only item in the list */ \
    dll_assert( *l == i1 ); \
    *l = i2; \
  } \
  else { \
    i2->prev = i1->prev; \
    i2->next = i1->next; \
    i2->prev->next = i2; \
    i2->next->prev = i2; \
    i1->prev = i1; \
    i1->next = i1; \
    if (*l == i1) *l = i2; \
  } \
} \
/* Detatch all the items between the head and last from the list */ \
static inline void dll_detach_##T##s_until( T * volatile*l, T *last ) { \
  T *first = *l; \
  if (last->next == first) { \
    /* Removing whole list */ \
    *l = 0; /* now empty */ \
  } \
  else { \
    T *new_head = last->next; \
    *l = new_head; \
    new_head->prev = first->prev; \
    first->prev->next = new_head; \
    last->next = first; \
    first->prev = last; \
  } \
} \
static inline void dll_insert_##T##_list_at_head( T *insert, T * volatile*l ) { \
  T *old_head = *l; \
  if (old_head != 0) { \
    T *old_last = old_head->prev; \
    T *last = insert->prev; \
    last->next = old_head; \
    old_head->prev = last; \
    insert->prev = old_last; \
    old_last->next = insert; \
  } \
 \
  *l = insert; \
} \
static inline T* T##_pool( T *(*alloc)( int size ), int number ) \
{ \
  T *result = alloc( number * sizeof( T ) ); \
  if (result != 0) { \
    for (int i = 0; i < number; i++) { \
      result[i].next = &result[i+1]; \
      result[i].prev = &result[i-1]; \
    } \
    result[number-1].next = &result[0]; \
    result[0].prev = &result[number-1]; \
  } \
  return result; \
}

