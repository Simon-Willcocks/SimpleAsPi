/* Copyright 2021 Simon Willcocks
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

typedef struct workspace workspace;

struct workspace {
  uint32_t lock;
  uint32_t output_pipe;
  uint32_t stack[60];
};

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

//NO_start;
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

const char title[] = "TestLocks";
const char help[] = "TestLocks\t0.01 (" CREATION_DATE ")";

UART volatile *const uart  = (void*) 0x7000;

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

// Replace this GPIO stuff with SWIs to:
//  claim pins 8 & 10 (GPIO 14 & 15)
//  Set their state to Alt0
#include "Devices/bcm_gpio.h"

GPIO volatile *const gpio  = (void*) 0x6000;

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

static inline
void wait_until_idle( UART *uart )
{
  while (UART_TxEmpty != (uart->flags & (UART_Busy | UART_TxEmpty))) {
    Task_Yield();
  }
}

static inline
void wait_for_space( UART *uart )
{
  while (0 != (uart->flags & UART_TxFull)) {
    Task_Yield();
  }
}

void send( uint32_t handle, char c, uint32_t *lock )
{
  for (;;) {
    Task_LockClaim( lock, handle );
    wait_until_idle( uart );
    core_info info = Task_Cores();
    //uart->data = 'a' + info.current;
    //uart->data = c;
    Task_LockRelease( lock );
    for (int i = 0; i < 8000; i++) asm ( "" );
  }
}

void c_start_tasks( uint32_t handle, uint32_t *lock )
{
  uint32_t uart_page = 0x3f201000 >> 12;
  Task_MapDevicePages( uart, uart_page, 1 );

  setup_pins();

  // Should read or set the clock frequency, but this value is set
  // in config.txt, for now.
  initialise_PL011_uart( uart, 3000000, 115200 );

  wait_until_idle( uart );

  uart->data = '\n';
  uart->data = 'A';
  uart->data = 'B';

  Task_LockClaim( lock, handle );

  uart->data = 'C';

  if (0) for (int i = 9; i >= 0; i--) {
    register void *start asm( "r0" ) = send;
    register uint32_t sp asm( "r1" ) = 0x9000 - 0x100 * i;
    register char r1 asm( "r2" ) = '0' + i;
    register uint32_t *r2 asm( "r3" ) = lock;

    register uint32_t handle asm( "r0" );

    asm volatile ( "svc %[swi]" // volatile in case we ignore output
      : "=r" (handle)
      : [swi] "i" (OSTask_Create)
      , "r" (start)
      , "r" (sp)
      , "r" (r1)
      , "r" (r2)
      : "lr", "cc" );
  }

  // Allow the tasks to start (they're queued on this core and won't run 
  // until this task yields, sleeps, or blocks waiting for a lock).
  // Soon, (not necessarily after the Yield returns) they will all be
  // blocked on the lock.
  Task_Yield();

  uart->data = 'D';

  Task_LockRelease( lock );

  uart->data = 'E';
  Task_Yield();

  for (int i = 0; i < 1000; i++) {
    //uart->data = '+';
    Task_Sleep( 10 );
    //Task_Yield();
  }
  uart->data = 'F';
  asm ( "dsb" );

  asm ( "bkpt 8" );
}

void __attribute__(( naked )) start_tasks( uint32_t handle, uint32_t *lock )
{
  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  c_start_tasks( handle, lock );
}

void __attribute__(( noinline )) c_init( uint32_t *private,
                                         char const *env,
                                         uint32_t instantiation )
{
  register void *start asm( "r0" ) = start_tasks;
  register uint32_t sp asm( "r1" ) = 0xddd00000;
  register workspace *r1 asm( "r2" ) = private;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void __attribute__(( naked )) init()
{
  register uint32_t *private asm ( "r12" );
  register char const *env asm ( "r10" );
  register uint32_t instantiation asm ( "r11" );

  // Move r12 into argument register
  asm volatile ( "push { lr }" );

  c_init( private, env, instantiation );

  asm ( "pop { pc }" );
}

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}

void __attribute__(( noinline, noreturn )) Logging()
{
  for (;;) {
    Task_Sleep( 1000 );
  asm ( "bkpt 9" );
  }
}

void start()
{
  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  Logging();
}
