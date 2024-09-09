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
#include "ostaskops.h"
#include "bcm_uart.h"

// Note for when the UART code is written; associated interrupt is 57

typedef struct workspace workspace;

struct workspace {
  uint32_t output_pipe;
  uint32_t stack[60];
};

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "PL011UART";
const char help[] = "PL011UART\t0.01 (" CREATION_DATE ")";

// TEMPORARY GPIO stuff is at 0x2000
UART volatile *const uart  = (void*) 0x1000;

// Hopefully no stack at all...
__attribute__(( optimize( "O4" ), naked, noreturn ))
void send_to_uart( uint32_t handle, uint32_t pipe )
{
  Task_EnablingInterrupts();
  qa7->Core_Mailboxes_Interrupt_control[core] = 1;
  int count = 0;

  for (;;) {
    if ((count++ & 0xfff) == 0) *(uint32_t*) 0xfffff000 = '0' + core;

    register uint32_t num asm ( "r0" ) = 0x84; // Mailbox 0 interrupt
    asm ( "svc 0x1001" : : "r" (num) );

    qa7->Core_write_clear[core].Mailbox[0] = 0xffffffff;
  }
}

void send_to_uart( char const *string, uint32_t length )
{
  for (int i = 0; i < length; i++) {
    // Send to UART (this should work on qemu, but not on real hardware;
    // that needs to wait for ready to send interrupts
    // Note: this is writing a whole word to the UART, could be a
    // problem.
    // The real thing will also have to claim the appropriate GPIO pins
    // and set the alternate functions.
    // FIXME Yielding or sleeping breaks something!
    while (0 != (uart->flags & UART_TxFull)) { for (int i = 0; i < 10000; i++) asm ( "" : : "r"(i) ); }
     // 
    uart->data = string[i];
    ensure_changes_observable();
  }
}

// Replace this GPIO stuff with SWIs to:
//  claim pins 8 & 10 (GPIO 14 & 15)
//  Set their state to Alt0
#include "Devices/bcm_gpio.h"

GPIO volatile *const gpio  = (void*) 0x2000;

static inline void setup_pins()
{
  uint32_t page = 0x3f200000 >> 12;
  Task_MapDevicePages( gpio, page, 1 );

  set_state( gpio, 14, GPIO_Alt0 );
  set_state( gpio, 15, GPIO_Alt0 );
}


// Ensure the GPIO pins are allocated to the serial port outside
// this routine.

// freq is the reference clock frequency
static inline
void initialise_PL011_uart( UART volatile *uart, uint32_t freq, uint32_t baud )
{
  // TODO: block when the FIFO is full, wait for interrupts, etc.

  // FIXME remove this delay, intended to let the other core run in qemu,
  // like I suspect is happening in real hardware
  for (int i = 0; i < 0x1000000; i++) asm ( "" );

  // Disable UART
  uart->control &= ~1;
  asm ( "dsb" );

  // Wait for current byte to be transmitted/received

  while (0 != (uart->flags & UART_Busy)) { }

  // UART clock is clock 2, rather than use the mailbox interface for
  // this test, put init_uart_clock=3000000 in config.txt

  uint32_t const ibrd = freq / (16 * baud);
  // The top 6 bits of the fractional part...
  uint32_t const fbrdx2 = ((8 * freq) / baud) & 0x7f;
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

  // No interrupts, for the time being, transmit only
  uart->control = uart_enable | transmit_enable;
  asm ( "dsb" );
}

void manage( uint32_t handle, workspace *ws )
{
  uint32_t uart_page = 0x3f201000 >> 12;
  Task_MapDevicePages( uart, uart_page, 1 );

  // Open UART

  // TODO: Use a GPIO module
  setup_pins();

  // Should read or set the clock frequency, but this value is set
  // in config.txt, for now. Also, baud rate from client task...
  // TODO
  initialise_PL011_uart( uart, 3000000, 115200 );

  uint32_t output_pipe = ws->output_pipe;

  {
    register char *var asm( "r0" ) = "PL011UART";
    register uint32_t val asm( "r1" ) = &output_pipe;
    register uint32_t len asm( "r2" ) = 4;
    register uint32_t ctx asm( "r3" ) = 0;
    register uint32_t type asm( "r4" ) = 1;
    asm ( "svc 0x24" : 
        : "r" (var)
        , "r" (val)
        , "r" (len)
        , "r" (ctx)
        , "r" (type)
        );
  }

  for (;;) {
    PipeSpace data = PipeOp_WaitForData( output_pipe, 1 );
    if (data.available == 0) {
      // TODO make available for claiming again
      PipeOp_SetSender( output_pipe, 0 );
    }

    while (data.available != 0) {
      char *string = data.location;
      send_to_uart( string, data.available );
      data = PipeOp_DataConsumed( output_pipe, data.available );
    }
  }
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  workspace *ws = rma_claim( sizeof( workspace ) );
  *private = ws;

  ws->output_pipe = PipeOp_CreateForTransfer( 4096 );
  PipeOp_SetSender( ws->output_pipe, 0 );

  register void *start asm( "r0" ) = manage;
  register uint32_t sp asm( "r1" ) = aligned_stack( ws + 1 );
  register workspace *r1 asm( "r2" ) = ws;
  register uint32_t new_handle asm( "r0" );
  asm volatile ( // volatile because we ignore output
        "svc %[swi_spawn]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (new_handle)
    , "=r" (sp) // corrupted
    : [swi_spawn] "i" (OSTask_Spawn)
    , [swi_release] "i" (OSTask_ReleaseTask)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void __attribute__(( naked )) init()
{
  register struct workspace **private asm ( "r12" );
  register char const *env asm ( "r10" );
  register uint32_t instantiation asm ( "r11" );

  // Move r12 into argument register
  asm volatile ( "push { lr }" );

  c_init( private, env, instantiation );

  asm ( "pop { pc }" );
}

