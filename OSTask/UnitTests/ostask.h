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

#include "CK_types.h"
#include "mpsafe_dll.h"

void _exit( int );

#define PANIC _exit( 1 )
#define assert( x ) if (!(x)) PANIC; else

static inline
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

struct shared {
  struct {
    OSTask *runnable;
    OSTask *sleeping;
  } ostask;
} shared;


MPSAFE_DLL_TYPE( OSTask );

void sleeping_tasks_add( OSTask *tired );
void sleeping_tasks_tick();

