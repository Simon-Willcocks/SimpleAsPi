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

