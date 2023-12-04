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
 \
static inline void mpsafe_insert_##T##_at_tail( T **head, T *item ) \
{ \
  /* TODO add SEV/WFE */ \
  for (;;) { \
    T *old = *head; \
    uint32_t uold = (uint32_t) old; \
    if (uold == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (old == 0) { \
      /* Empty list, but another core might add something... */ \
      if (0 == change_word_if_equal( (uint32_t*) head, 0, (uint32_t) item )) { \
        signal_event(); return; \
      } \
    } \
    else if (uold != 1 \
          && uold == change_word_if_equal( (uint32_t*) head, uold, 1 )) { \
      /* Replaced head pointer with 1, can work on list safely... */ \
      dll_assert( uold != 1 ); \
      dll_attach_##T( item, &old ); \
      uint32_t one = change_word_if_equal( (uint32_t*) head, 1, (uint32_t) old->next ); \
      dll_assert( 1 != one ); one = one; \
      signal_event(); return; \
    } \
  } \
} \
 \
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
        signal_event(); return; \
      } \
    } \
    else if (uold != 1 \
          && uold == change_word_if_equal( (uint32_t*) head, uold, 1 )) { \
      dll_attach_##T( item, &old ); \
      uint32_t one = change_word_if_equal( (uint32_t*) head, 1, (uint32_t) old ); \
      dll_assert( 1 != one ); one = one; \
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
        signal_event(); return; \
      } \
    } \
    else if (uold != 1 \
          && uold == change_word_if_equal( (uint32_t*) head, uold, 1 )) { \
      T *tail = old->next; \
      dll_attach_##T( item, &tail ); \
      uint32_t one = change_word_if_equal( (uint32_t*) head, 1, (uint32_t) old ); \
      dll_assert( 1 != one ); one = one; \
      signal_event(); return; \
    } \
  } \
} \
 \
static inline T *mpsafe_manipulate_##T##_list_returning_item( T **head, T *(*update)( T** a, void *p ), void *p ) \
{ \
  for (;;) { \
    T *head_item = *head; \
    uint32_t uhead_item = (uint32_t) head_item; \
    if (uhead_item == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (uhead_item == change_word_if_equal( (uint32_t*) head, uhead_item, 1 )) { \
      /* Replaced head pointer with 1, can work on list safely. (May be empty!) */ \
      T *result = update( &head_item, p ); \
      *head = head_item; /* Release list */ \
      signal_event(); return result; \
    } \
  } \
  return 0; \
} \
static inline void *mpsafe_manipulate_##T##_list( T **head, void *(*update)( T** a, void *p ), void *p ) \
{ \
  uint32_t *list = (uint32_t*) head; \
  for (;;) { \
    push_writes_to_cache(); /* Ensure we can see updates made by other cores */ \
    T *head_item = *head; \
    uint32_t uhead_item = (uint32_t) head_item; \
    if (uhead_item == 1) { \
      /* Another core is manipulating the list, try again later */ \
      wait_for_event(); \
    } \
    else if (uhead_item == change_word_if_equal( list, uhead_item, 1 )) { \
      /* Replaced head pointer with 1, can work on list safely. */ \
      /* The list may be empty! */ \
      void *result = update( &head_item, p ); \
      ensure_changes_observable(); \
      *head = head_item; /* Release list */ \
      ensure_changes_observable(); \
      signal_event(); \
      return result; \
    } \
  } \
  return 0; \
} \
static inline void *DO_NOT_USE_detach_##T##_head( T **head, void *p ) \
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
static inline void *DO_NOT_USE_detach_##T( T **head, void *p ) \
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
/* Returns the old head of the list, replacing it with the next or null */ \
static inline T *mpsafe_detach_##T##_at_head( T **head ) \
{ \
  return mpsafe_manipulate_##T##_list( head, DO_NOT_USE_detach_##T##_head, 0 ); \
} \
static inline void mpsafe_detach_##T( T **head, T *t ) \
{ \
  mpsafe_manipulate_##T##_list( head, DO_NOT_USE_detach_##T, t ); \
}
