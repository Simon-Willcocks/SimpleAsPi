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

typedef struct OSTask_extras OSTask_extras;
typedef struct OSTaskSlot_extras OSTaskSlot_extras;

struct OSTask_extras {
};

struct OSTaskSlot_extras {
  struct {
    uint32_t base;      // Pages
    uint32_t pages;     // Pages
    uint32_t va;        // Absolute
  } app_mem[30];
};

// Functions required by the OSTask code

#include "heap.h"

// Return a 4-byte aligned pointer to an area of at least
// size bytes of privileged writable memory. Or NULL.
// Will not be called until the OSTask subsystem has called startup.
static inline void *system_heap_allocate( uint32_t size )
{
  extern uint8_t system_heap_base;
  return heap_allocate( &system_heap_base, size );
}

// Ditto, except usr accessible and executable memory
static inline void *shared_heap_allocate( uint32_t size )
{
  extern uint8_t shared_heap_base;
  return heap_allocate( &shared_heap_base, size );
}

// Free memory allocated using one of the above
static inline void system_heap_free( void *block )
{
  extern uint8_t system_heap_base;
  heap_free( &system_heap_base, block );
}

static inline void shared_heap_free( void *block )
{
  extern uint8_t shared_heap_base;
  heap_free( &shared_heap_base, block );
}

