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

// This routine, provided by another subsystem, will be entered with the
// lowest 16MiB of memory mapped at VA 0, and workspace allocated for each
// core. The lowest 16KiB contains a Level 1 Translation Table, the top
// of the workspace is the initial boot stack.

extern uint32_t const boot_mem;

void __attribute__(( noreturn )) boot_with_stack( uint32_t core, 
                                   void *workspace, uint32_t size );

// Memory management:
// 32-bit page numbers can deal with up to 16,384 GiB of RAM

typedef enum {  CK_MemoryRWX,
                CK_MemoryRW,
                CK_MemoryRX,
                CK_MemoryR,
                CK_Device } CK_Memory;

// Handlers either use one of the map_... functions to remove
// the fault or return false if there's nothing it can do.
// (One possible action to remove the fault is to replace the
// running task with one that will not fault. This code doesn't
// have to know about that.)
typedef bool (*memory_fault_handler)( uint32_t va, uint32_t fault );

void clear_memory_region(
                uint32_t *translation_table,
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler );

void map_app_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                CK_Memory memory_type );

void map_global_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                CK_Memory memory_type );

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

// TODO:
// push writes to RAM (va range)
// RAM may have changed (va range) - invalidate cache

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

      ensure_changes_observable();
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

