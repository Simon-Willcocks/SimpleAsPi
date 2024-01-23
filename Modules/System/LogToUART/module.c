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

UART volatile *const uart  = (void*) 0x7000;

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

void transfer_to_output( uint32_t me, PipeSpace data, workspace *ws, int core )
{
  assert( data.available != 0 );

  bool reclaimed = Task_LockClaim( &ws->lock, me );

  if (!reclaimed) {
    // You might think this would be necessary, but as long as you're sure
    // the owner is currently 0, the first wait call will set the current
    // task as the sender (or receiver on WaitForData).
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

  char const *s = data.location;
  do {
    PipeSpace space = PipeOp_WaitForSpace( ws->output_pipe, data.available );
    // We might get less space available than data.available if the output
    // pipe is too small. We can work with that, but this task may block...

    while (space.available != 0 && i < data.available) {
      char *d = space.location;
      int n = 0; // Offset from start of space
      while (n < space.available && i < data.available) {
        d[n++] = s[i++];
      }
      space = PipeOp_SpaceFilled( ws->output_pipe, n );
    }
  } while (i < data.available);

  if (!reclaimed) {
    PipeOp_SetSender( ws->output_pipe, 0 );
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
      data = PipeOp_DataConsumed( pipe, data.available );
    }
  }
}

void send_to_uart( char const *string, uint32_t length )
{
#ifndef QEMU
  return; // Testing 19/1/2024 seems to fail on real hardware
#else
  for (int i = 0; i < length; i++) {
    // Send to UART (this should work on qemu, but not on real hardware;
    // that needs to wait for ready to send interrupts
    // Note: this is writing a whole word to the UART, could be a
    // problem.
    // The real thing will also have to claim the appropriate GPIO pins
    // and set the alternate functions.
    uart->data = string[i];
  }
#endif
}

void start_log( uint32_t handle, workspace *ws )
{
  // This location should be passed to the final driver module by the HAL
  uint32_t uart_page = 0x3f201000 >> 12;
  Task_MapDevicePages( uart, uart_page, 1 );

  send_to_uart( "Starting", 9 );

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

  for (;;) {
    PipeSpace data = PipeOp_WaitForData( ws->output_pipe, 1 );

    while (data.available != 0) {
      char *string = data.location;
      send_to_uart( string, data.available );
      data = PipeOp_DataConsumed( ws->output_pipe, data.available );
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

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
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
