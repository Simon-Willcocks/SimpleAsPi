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

// This routine, to be provided by another subsystem, will be entered with
// the lowest 16MiB of memory mapped at VA 0, and workspace allocated for
// each core. The first workspace_needed_for_empty_translation_tables bytes
// contain translation tables, the top of the workspace is the initial boot
// stack.
// There are no processor vectors set up at this time, and the stack
// is in the low memory RAM.

void __attribute__(( noreturn )) boot_with_stack( uint32_t core );

#include "mmu.h"

// Memory write utilities

// The function names are what I intend the routine to achieve

static inline void push_writes_to_device()
{
  // Assumes device memory is CK_Device
  asm ( "dsb" );
}

static inline void ensure_changes_observable()
{
  // Assumes device memory is normal memory
  asm ( "dmb" );
}

static inline void push_writes_to_cache()
{
  // Assumes device memory is normal memory
  asm ( "dsb" );
}

static inline void signal_event()
{
  asm ( "sev" );
}

static inline void wait_for_event()
{
  asm ( "wfe" );
}

uint32_t number_of_cores();

#define PANIC do { asm ( "bkpt %[line]\n wfi" : : [line] "i" (__LINE__) ); for (;;) {} } while (true)

// TODO:
// push writes to RAM (va range)
// RAM may have changed (va range) - invalidate cache

void push_writes_out_of_cache( uint32_t va, uint32_t size );

void RAM_may_have_changed( uint32_t va, uint32_t size );

// Multi-processing primitives. No awareness of OSTasks.

// Change the word at `word' to the value `to' if it contained `from'.
// Returns the original content of word (== from, if changed successfully)
static inline
uint32_t change_word_if_equal( uint32_t *word, uint32_t from, uint32_t to )
{
  uint32_t failed = true;
  uint32_t value;

  do {
    asm volatile ( "ldrex %[value], [%[word]]"
                   : [value] "=&r" (value)
                   : [word] "r" (word) );

    if (value == from) {
      // Assembler note:
      // The failed and word registers are not allowed to be the same, so
      // pretend to gcc that the word may be written as well as read.

      asm volatile ( "strex %[failed], %[value], [%[word]]"
                     : [failed] "=&r" (failed)
                     , [word] "+r" (word)
                     : [value] "r" (to) );

      if (!failed) ensure_changes_observable();
    }
    else {
      asm ( "clrex" );
      break;
    }
  } while (failed);

  return value;
}

// core_claim_lock returns true if this core already owns the lock.
// It is assumed that the lock is unlocked iff it contains zero.

// Suggested usage:
//  bool reclaimed = core_claim_lock( &lock, core_number + 1 );
//  ...
//  if (!reclaimed) core_release_lock( &lock );

// See Barrier_Litmus_Tests_and_Cookbook_A08, section 7

static inline
bool core_claim_lock( uint32_t *lock, uint32_t value )
{
  for (;;) {
    uint32_t core = value;
    uint32_t old = change_word_if_equal( lock, 0, core );
    if (old == 0) return false;
    if (old == core) return true;
    wait_for_event();
  }
}

static inline
void core_release_lock( uint32_t volatile *lock )
{
  ensure_changes_observable();
  *lock = 0;
  push_writes_to_cache();
  signal_event();
}

// To satisfy the optimiser in gcc:
void *memset(void *s, int c, size_t n);

#define C_CLOBBERED "r0-r3,r12"

static inline uint32_t get_svc_number( uint32_t lr )
{
  // lr is the address of the instruction following the svc
  uint32_t result;
  asm ( "ldr %[r], [%[next], #-4]" : [r] "=r" (result) : [next] "r" (lr) );
  return result & 0x00ffffff;
}
