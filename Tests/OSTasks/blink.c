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
#include "ostask.h"
#include "qa7.h"
#include "bcm_gpio.h"

// If you connect a LED and roughly 1kOhm resistor in series to
// GPIO pins 22 and 27 (physical pins 15 and 13, respectively)
// down to ground (e.g. physical pin 1), this will alternately
// blink them.

void Sleep( uint32_t ms )
{
  register uint32_t t asm( "r0" ) = ms;
  asm ( "svc %[swi]" : : [swi] "i" (OSTask_Sleep), "r" (t) );
}

void __attribute__(( noreturn )) startup()
{
  // Running with multi-tasking enabled. This routine gets called
  // just once.
  QA7 *qa7 = (void*) 0xfff00000;

  memory_mapping map_qa7 = {
    .base_page = 0x40000000 >> 12,
    .pages = 1,
    .vap = qa7,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &map_qa7 );

  GPIO *gpio = (void*) 0xfff01000;

  memory_mapping map_gpio = {
    .base_page = 0x3f200000 >> 12,
    .pages = 1,
    .vap = gpio,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &map_gpio );

  uint32_t yellow = 27;
  uint32_t green = 22;

  set_state( gpio, yellow, GPIO_Output );
  set_state( gpio, green, GPIO_Output );
  push_writes_to_device();

  gpio->gpclr[yellow/32] = 1 << (yellow % 32);
  gpio->gpclr[green/32] = 1 << (green % 32);
  push_writes_to_device();

  for (;;) {
    Sleep( 1000 );
    for (int i = 0; i < 1 << 24; i++) asm ( "nop" );
    // gpio->gpset[yellow/32] = 1 << (yellow % 32);
    gpio->gpclr[green/32] = 1 << (green % 32);
    push_writes_to_device();
    for (int i = 0; i < 1 << 25; i++) asm ( "nop" );
    // gpio->gpclr[yellow/32] = 1 << (yellow % 32);
    gpio->gpset[green/32] = 1 << (green % 32);
    push_writes_to_device();
  }

  __builtin_unreachable();
}

