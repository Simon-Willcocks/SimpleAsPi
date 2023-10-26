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
extern uint32_t const boot_mem;

// If you connect a LED and roughly 1kOhm resistor in series to
// GPIO pins 22 and 27 (physical pins 15 and 13, respectively)
// down to ground (e.g. physical pin 1), this will alternately
// blink them.

void set_way_no_CCSIDR2()
{
  asm ( "dsb sy" );
  // Select cache level
  for (int level = 1; level <= 2; level++) {
    uint32_t size;
    asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" ((level-1) << 1) ); // CSSELR Selection Register.
    asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (size) ); // CSSIDR
    uint32_t line_size = ((size & 7)+4);
    uint32_t ways = 1 + ((size & 0xff8) >> 3);
    uint32_t sets = 1 + ((size & 0x7fff000) >> 13);
    int wayshift; // Number of bits to shift the way index by
    asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways - 1) );

    for (int way = 0; way < ways; way++) {
      uint32_t setway = (way << wayshift) | ((level - 1) << 1);
      for (int set = 0; set < sets; set++) {
        asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
      }
    }
  }

  asm ( "dsb sy" );
}


void __attribute__(( noreturn )) boot_with_stack( uint32_t core, void *workspace, uint32_t size )
{
  // Running with MMU enabled (allowing for locks to work)
  // but in low memory.

  uint32_t *core_l1tt = workspace;

  core_l1tt[boot_mem >> 20] = 0x3f230c12; // GPIO, Device-nGnRnE
  asm ( "dsb sy" ); // Essential! Otherwise the MMU doesn't see the value.

  uint32_t volatile *gpio = (void*) boot_mem;
  uint32_t mask = (7 << (3 * 7)) | (7 << (3 * 2));
  enum { GPIO_Input, GPIO_Output, GPIO_Alt5, GPIO_Alt4, GPIO_Alt0, GPIO_Alt1, GPIO_Alt2, GPIO_Alt3 };

  uint32_t types = (GPIO_Output << (3 * 7)) | (GPIO_Output << (3 * 2));
  gpio[2] = (gpio[2] & ~mask) | types;
  asm ( "dsb sy" );

  uint32_t bits = (1 << 27) | (1 << 22);
  uint32_t one_bit = (1 << 27);

  for (;;) {
    gpio[0x1c/4] = one_bit;
    one_bit = one_bit ^ bits;
    gpio[0x28/4] = one_bit;
    asm ( "dsb sy" );
    for (int i = 0; i < 50000000; i++) asm( "nop" );
  }

  __builtin_unreachable();
}

