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
#include "bcm_uart.h"
#include "bcm_gpio.h"

static inline
void initialise_PL011_uart( UART volatile *uart )
{
  // Ensure the GPIO pins are allocated to the serial port
  // That's the default, the above is just to remind me when
  // this code goes into a module.

  // Disable UART
  uart->control &= ~1;
  asm ( "dsb" );

  // Wait for current byte to be transmitted/received

  while (0 != (uart->flags & (1 << 3))) { }

  // UART clock is clock 2, rather than use the mailbox interface for
  // this test, put init_uart_clock=3000000 in config.txt

  uint32_t const reference_clock_freq = 3000000; // 3MHz
  uint32_t const baud_rate = 115200;

  uint32_t const ibrd = reference_clock_freq / (16 * baud_rate);
  // The top 6 bits of the fractional part...
  uint32_t const fbrdx2 = ((8 * reference_clock_freq) / baud_rate) & 0x7f;
  // rounding off...
  uint32_t const fbrd = (fbrdx2 + 1) / 2;

  uart->integer_baud_rate_divisor = ibrd;
  uart->fractional_baud_rate_divisor = fbrd;

  uint32_t const eight_bits = (3 << 5);
  uint32_t const fifo_enable = (1 << 4);
  uint32_t const parity_enable = (1 << 1);
  uint32_t const even_parity = parity_enable | (1 << 2);
  uint32_t const odd_parity = parity_enable | (0 << 2);
  uint32_t const one_stop_bit = 0;
  uint32_t const two_stop_bits = (1 << 3);

  uart->line_control = (eight_bits | one_stop_bit | fifo_enable);

  uart->interrupt_mask = 0;

  uint32_t const transmit_enable = (1 << 8);
  uint32_t const receive_enable = (1 << 9);
  uint32_t const uart_enable = 1;

  // No interrupts, for the time being
  uart->control = uart_enable | transmit_enable;
  asm ( "dsb" );
}

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

  GPIO volatile *const gpio = (void*) 0xffffe000;
  UART volatile *const uart  = (void*) 0xfffff000;

  {
  memory_mapping mapping = {
    .base_page = 0x3f200000 >> 12,
    .pages = 1,
    .vap = gpio,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &mapping );
  }

  {
  memory_mapping mapping = {
    .base_page = 0x3f201000 >> 12,
    .pages = 1,
    .vap = uart,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &mapping );
  }

  set_state( gpio, 27, GPIO_Output );
  set_state( gpio, 22, GPIO_Output );

  set_state( gpio, 14, GPIO_Alt0 );
  set_state( gpio, 15, GPIO_Alt0 );

  initialise_PL011_uart( uart );

  uint32_t bits = (1 << 27) | (1 << 22);
  uint32_t delay = 50000000;

  delay = delay / (core + 1);

  char c = ' ';
  for (;;) {
    while (0 != (uart->flags & (1 << 3))) { }
    // uart->data = 'Q';
    uart->data = c++;
    push_writes_to_device();
    gpio->gpset[0] = bits;
    push_writes_to_device();
    for (int i = 0; i < delay; i++) asm( "nop" );
    while (0 != (uart->flags & (1 << 3))) { }
    // uart->data = 'W';
    push_writes_to_device();
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
