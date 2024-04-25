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

// Note for when the UART code is written; associated interrupt is 57


void send_to_uart( char const *string, uint32_t length );

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

const char title[] = "LogToUART";
const char help[] = "LogToUART\t0.01 (" CREATION_DATE ")";

UART volatile *const uart  = (void*) 0x4000;

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

void transfer_to_output( uint32_t me, PipeSpace data, workspace *ws, int core )
{
  assert( data.available != 0 );

  bool reclaimed = Task_LockClaim( &ws->lock, me );

  if (!reclaimed) {
    // You might think SetSender would be necessary, but as long as you're
    // sure the owner is currently 0, the first wait call will set the
    // current task as the sender (or receiver on WaitForData).
    // PipeOp_SetSender( ws->output_pipe, me );

    // Colour by core number \033[01;32m
    // High number cores are going to look odd, but the first 16 should be
    // distinguishable

    char colour[8];
    colour[0] = '\033';
    colour[1] = '[';
    colour[2] = '0' + ((core >> 6) & 7);
    colour[3] = '0' + ((core >> 3) & 7);
    colour[4] = ';';
    colour[5] = '3';
    colour[6] = '7' - (core & 7);
    colour[7] = 'm';
    PipeSpace data = { .location = colour, .available = sizeof( colour ) };

    transfer_to_output( me, data, ws, core );
  }

  uint32_t i = 0;

  uint32_t output_pipe = ws->output_pipe;

/* confused things...
char num[9];
num[0] = '0' + 0xf & (data.available >> 28);
num[1] = '0' + 0xf & (data.available >> 24);
num[2] = '0' + 0xf & (data.available >> 20);
num[3] = '0' + 0xf & (data.available >> 16);
num[4] = '0' + 0xf & (data.available >> 12);
num[5] = '0' + 0xf & (data.available >> 8);
num[6] = '0' + 0xf & (data.available >> 4);
num[7] = '0' + 0xf & (data.available >> 0);
num[8] = '\n';
send_to_uart( num, 9 );
*/

  char const *s = data.location;
  do {
    PipeSpace space = PipeOp_WaitForSpace( output_pipe, data.available );
    // We might get less space available than data.available if the output
    // pipe is too small. We can work with that, but this task may block...

if (space.available < data.available) asm ( "bkpt 888" );

    while (space.available != 0 && i < data.available) {
      char *d = space.location;
      int n = 0; // Offset from start of space
      while (n < space.available && i < data.available) {
        d[n++] = s[i++];
      }
      space = PipeOp_SpaceFilled( output_pipe, n );
    }
  } while (i < data.available);

  if (!reclaimed) {
    PipeOp_SetSender( output_pipe, 0 );
    // The next owner of the lock can be sure the sender is unset.
    Task_LockRelease( &ws->lock );
  }
}

void core_debug_task( uint32_t handle, int core, workspace *ws, uint32_t pipe )
{
  Task_LogString( "Waiting for log ", 16 );

  Task_LogHex( pipe );
  Task_LogString( "\n", 1 );
  for (;;) {
    PipeSpace data = PipeOp_WaitForData( pipe, 1 );
    while (data.available != 0) {
      transfer_to_output( handle, data, ws, core );
if ((ws->lock & ~1) == handle) asm ( "bkpt 999" );
      data = PipeOp_DataConsumed( pipe, data.available );
    }
  }
}

void hitit( uint32_t handle )
{
  register int n = 20;
  for (;;) {
    Task_Sleep( 1000 );
    while (UART_TxEmpty != (uart->flags & UART_TxEmpty)) Task_Yield();
    uart->data = '+';
    if (--n == 0) { asm ( "bkpt 44" ); }
  }
}

void send_to_uart( char const *string, uint32_t length )
{
#ifdef DEBUG__NO_SEND_TO_UART
  return;
#else
  for (int i = 0; i < length; i++) {
    // FIXME Yielding or sleeping breaks something!
    while (0 != (uart->flags & UART_TxFull)) { for (int i = 0; i < 10000; i++) asm ( "" : : "r"(i) ); }
     // 
    uart->data = string[i];
    ensure_changes_observable();
  }
#endif
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

  uart->interrupt_fifo_level_select = 0;
  uart->interrupt_mask = 0;

  uint32_t const transmit_enable = (1 << 8);
  uint32_t const receive_enable = (1 << 9);
  uint32_t const uart_enable = 1;

  // No interrupts, for the time being, transmit only
  uart->control = uart_enable | transmit_enable;
  asm ( "dsb" );
}


void start_log( uint32_t handle, workspace *ws )
{
  // This location should be passed to the final driver module by the HAL
  // Not really. The serial port should be taken out of this altogether
  uint32_t uart_page = 0x3f201000 >> 12;
  Task_MapDevicePages( uart, uart_page, 1 );

  setup_pins();

  // Should read or set the clock frequency, but this value is set
  // in config.txt, for now.
  initialise_PL011_uart( uart, 3000000, 115200 );


  send_to_uart( "Starting\n", 9 );
  {
  core_info cores = Task_Cores();
  char n = '0' + cores.total;
  send_to_uart( &n, 1 );
  send_to_uart( "\n", 1 );
  }

//  UART volatile * const uart = (void*) 0xfffff000;
  while (UART_TxEmpty != (uart->flags & (UART_Busy | UART_TxEmpty))) { for (int i = 0; i < 1000; i++) asm ( "" ); }

  ws->output_pipe = PipeOp_CreateForTransfer( 4096 );

  // Before creating any of the tasks that might want to use it...
  PipeOp_SetSender( ws->output_pipe, 0 );

  // Note: This loop ends up with this routine running on the last core
  // every time, perhaps it would be better to start with the current
  // core and wrap around to zero, ending up back on the core we started
  // on?
  core_info cores = Task_Cores();

  for (int i = 0; i < cores.total; i++) {
    uint32_t const stack_size = 256;
    uint8_t *stack = rma_claim( stack_size );

    // Note: The above rma_claim may (probably will) result in the core
    // we're running on to change.
    // Therefore we have to run it before switching (or switch more than
    // once).
  
    Task_SwitchToCore( i );

    uint32_t pipe = Task_GetLogPipe();

    Task_LogHex( pipe ); Task_LogNewLine();
    
    if (pipe != 0) {
      register void *start asm( "r0" ) = core_debug_task;
      register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
      register uint32_t r1 asm( "r2" ) = i;
      register workspace *r2 asm( "r3" ) = ws;
      register uint32_t r3 asm( "r4" ) = pipe;

      register uint32_t handle asm( "r0" );

      asm volatile ( "svc %[swi]" // volatile in case we ignore output
        : "=r" (handle)
        : [swi] "i" (OSTask_Create)
        , "r" (start)
        , "r" (sp)
        , "r" (r1)
        , "r" (r2)
        , "r" (r3)
        : "lr", "cc" );
    }
    else asm ( "bkpt 6" );
  }

  Task_LogString( "Log starting\n", 13 );


  if (0) {
    uint32_t const stack_size = 256;
    uint8_t *stack = rma_claim( stack_size );

    // THIS IS A HACK FIXME and delete asap...
    register void *start asm( "r0" ) = hitit;
    register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );

    register uint32_t handle asm( "r0" );

    asm volatile ( "svc %[swi]" // volatile in case we ignore output
      : "=r" (handle)
      : [swi] "i" (OSTask_Create)
      , "r" (start)
      , "r" (sp)
      : "lr", "cc" );
  }

  uint32_t output_pipe = ws->output_pipe;
  for (;;) {
    PipeSpace data = PipeOp_WaitForData( output_pipe, 1 );

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
  ws->lock = 0;
  ws->output_pipe = 0;

  register void *start asm( "r0" ) = start_log;
  register uint32_t sp asm( "r1" ) = aligned_stack( ws + 1 );
  register workspace *r1 asm( "r2" ) = ws;
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
  register struct workspace **private asm ( "r12" );
  register char const *env asm ( "r10" );
  register uint32_t instantiation asm ( "r11" );

  // Move r12 into argument register
  asm volatile ( "push { lr }" );

  c_init( private, env, instantiation );

  asm ( "pop { pc }" );
}

void __attribute__(( noreturn )) Logging()
{
  register uint32_t pin asm( "r0" ) = 27; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 200;
  register uint32_t off asm( "r2" ) = 100;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );

  for (;;) {
    Task_LogString( "Loggy ", 6 );
    Task_Sleep( 1000 );
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
