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

#ifndef dll_type
#include "doubly_linked_list.h"
#endif

#define MPSAFE_DLL_TYPE( T ) \
DLL_TYPE( T ) \
static inline void mpsafe_insert_##T##_at_head( T **head, T *item ) \
{ \
  for (;;) { \
    T *old = *head; \
    uint32_t uold = (uint32_t) old; \
    if (uold == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (old == 0) { \
      if (0 == change_word_if_equal( (uint32_t*) head, 0, (uint32_t) item )) { \
        push_writes_to_cache(); \
        signal_event(); return; \
      } \
    } \
    else if (uold != 1 \
          && uold == change_word_if_equal( (uint32_t*) head, uold, 1 )) { \
      dll_attach_##T( item, &old ); \
      uint32_t one = change_word_if_equal( (uint32_t*) head, 1, (uint32_t) old ); \
      dll_assert( 1 != one ); one = one; \
      push_writes_to_cache(); \
      signal_event(); return; \
    } \
  } \
} \
 \
static inline void mpsafe_insert_##T##_after_head( T **head, T *item ) \
{ \
  for (;;) { \
    T *old = *head; \
    uint32_t uold = (uint32_t) old; \
    if (uold == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (old == 0) { \
      if (0 == change_word_if_equal( (uint32_t*) head, 0, (uint32_t) item )) { \
        push_writes_to_cache(); \
        signal_event(); return; \
      } \
    } \
    else if (uold != 1 \
          && uold == change_word_if_equal( (uint32_t*) head, uold, 1 )) { \
      T *tail = old->next; \
      dll_attach_##T( item, &tail ); \
      uint32_t one = change_word_if_equal( (uint32_t*) head, 1, (uint32_t) old ); \
      dll_assert( 1 != one ); one = one; \
      push_writes_to_cache(); \
      signal_event(); return; \
    } \
  } \
} \
 \
static inline T *mpsafe_manipulate_##T##_list_returning_item( T *volatile *head, T *(*update)( T** a, void *p ), void *p ) \
{ \
  for (;;) { \
      push_writes_to_cache(); \
      ensure_changes_observable(); \
    T *head_item = *head; \
    uint32_t ui = (uint32_t) head_item; \
    if (ui == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (ui == change_word_if_equal( (uint32_t*) head, ui, 1 )) { \
      /* Replaced head pointer with 1, can work on list safely. */\
      /* The list may be empty! */ \
      T *result = update( &head_item, p ); \
      ensure_changes_observable(); \
      *head = head_item; /* Release list */ \
      push_writes_to_cache(); \
      signal_event(); \
      return result; \
    } \
  } \
  return 0; \
} \
static inline void mpsafe_manipulate_##T##_list( T *volatile *head, void (*update)( T** a, void *p ), void *p ) \
{ \
  for (;;) { \
      push_writes_to_cache(); \
      ensure_changes_observable(); \
    T *head_item = *head; \
    uint32_t ui = (uint32_t) head_item; \
    if (ui == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (ui == change_word_if_equal( (uint32_t*) head, ui, 1 )) { \
      /* Replaced head pointer with 1, can work on list safely. */ \
      /* The list may be empty! */ \
      update( &head_item, p ); \
      ensure_changes_observable(); \
      *head = head_item; /* Release list */ \
      push_writes_to_cache(); \
      signal_event(); \
      return; \
    } \
  } \
} \
static inline T *DO_NOT_USE_detach_##T##_head( T **head, void *p ) \
{ \
  T *h = *head; \
  if (h == 0) return 0; \
  *head = h->next; \
  if (*head == h) { \
    /* Already a single item list */ \
    *head = 0; \
  } \
  else { \
    dll_detach_##T( h ); \
  } \
  return h; \
} \
static inline T *DO_NOT_USE_detach_##T( T **head, void *p ) \
{ \
  T *i = p; \
  if (*head == i) { \
    *head = i->next; \
  } \
  if (*head == i) { \
    *head = 0; \
  } \
  else { \
    /* Not a single item list, not at head */ \
    dll_detach_##T( i ); \
  } \
  return i; \
} \
static inline void DO_NOT_USE_insert_tail_##T( T **head, void *p ) \
{ \
  if (*head == 0) { \
    *head = p; \
  } \
  else { \
    dll_attach_##T( p, head ); \
    *head = (*head)->next; \
  } \
} \
/* Returns the old head of the list, replacing it with the next or null */ \
static inline T *mpsafe_detach_##T##_at_head( T **head ) \
{ \
  return mpsafe_manipulate_##T##_list_returning_item( head, DO_NOT_USE_detach_##T##_head, 0 ); \
} \
static inline void mpsafe_insert_##T##_at_tail( T **head, T *item ) \
{ \
  mpsafe_manipulate_##T##_list( head, DO_NOT_USE_insert_tail_##T, item ); \
}
