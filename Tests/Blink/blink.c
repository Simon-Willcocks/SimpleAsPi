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

#include "CK_types.h"
#include "Processor/processor.h"

extern uint32_t boot_l1tt[];
extern uint32_t const initial_boot_memory_at_0;

// If you connect a LED and roughly 1kOhm resistor in series to
// GPIO pins 22 and 27 (physical pins 15 and 13, respectively)
// down to ground (e.g. physical pin 1), this will alternately
// blink them.

void __attribute__(( noreturn )) boot_with_stack( uint32_t core,
                                                  void *workspace,
                                                  uint32_t size )
{
  // Running with MMU enabled (allowing for locks to work)
  // but in low memory.
  static uint32_t lock = 0; // Note: only writable because ROM memory writable

  uint32_t *core_l1tt = workspace;
  uint32_t unmapped = initial_boot_memory_at_0;
  uint32_t const MiB_of_pages = 256;

  bool reclaimed = core_claim_lock( &lock, core + 1 );

  // Just temporary memory
  // At this stage, only MiB aligned allocations are possible,
  // the MMU code doesn't have a pool of level 2 translation 
  // tables yet.
  map_app_memory( core_l1tt, 0x3f200, MiB_of_pages, unmapped, CK_Device );

  uint32_t volatile *gpio = (void*) initial_boot_memory_at_0;
  uint32_t mask = (7 << (3 * 7)) | (7 << (3 * 2));
  enum { GPIO_Input, GPIO_Output, GPIO_Alt5, GPIO_Alt4, 
         GPIO_Alt0, GPIO_Alt1, GPIO_Alt2, GPIO_Alt3 };

  uint32_t types = (GPIO_Output << (3 * 7)) | (GPIO_Output << (3 * 2));
  gpio[2] = (gpio[2] & ~mask) | types;
  push_writes_to_device();

  core = 3 - core;
  uint32_t bits = ((core & 1) << 27) | ((core & 2) << 21); // Note: 21 because starting with 2
  uint32_t delay = 50000000;

  delay = delay / (core + 1);

  for (;;) {
    gpio[0x1c/4] = bits;
    push_writes_to_device();
    for (int i = 0; i < delay; i++) asm( "nop" );
    gpio[0x28/4] = bits;
    push_writes_to_device();
    for (int i = 0; i < delay; i++) asm( "nop" );
  }

  __builtin_unreachable();
}

