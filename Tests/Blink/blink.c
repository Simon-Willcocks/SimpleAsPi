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
#include "processor.h"

#include "Devices/bcm_gpio.h"

// If you connect a LED and roughly 1kOhm resistor in series to
// GPIO pins 22 and 27 (physical pins 15 and 13, respectively)
// down to ground (e.g. physical pin 1), this will alternately
// blink them.

void __attribute__(( noreturn )) boot_with_stack( uint32_t core )
{
  // Running in high memory with MMU enabled

  // The shared and core workspaces have been cleared before 
  // this routine is called

  forget_boot_low_memory_mapping();

  // So only one core blinks the LEDs
  core_claim_lock( &shared.boot_lock, core + 1 );

  // Just temporary memory
  // The MMU code doesn't have a pool of level 2 translation 
  // tables yet, but the top MiB is page-mappable.

  GPIO *gpio = (void*) 0xfff00000;

  memory_mapping map_gpio = {
    .base_page = 0x3f200000 >> 12,
    .pages = 1,
    .vap = gpio,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &map_gpio );

  map_memory( &map_gpio );

  set_state( gpio, 27, GPIO_Output );
  set_state( gpio, 22, GPIO_Output );

  uint32_t bits = (1 << 27) | (1 << 22);
  uint32_t delay = 50000000;

  delay = delay / (core + 1);

  for (;;) {
    gpio->gpset[0] = bits;
    push_writes_to_device();
    for (int i = 0; i < delay; i++) asm( "nop" );
    gpio->gpclr[0] = bits;
    push_writes_to_device();
    for (int i = 0; i < delay; i++) asm( "nop" );
  }

  __builtin_unreachable();
}

void claim_contiguous_memory()
{
  PANIC;
}
