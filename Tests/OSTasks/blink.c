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
#include "bcm_uart.h"
#include "heap.h"
#include "raw_memory_manager.h"

void setup_system_heap()
{
  extern uint8_t system_heap_base;
  extern uint8_t system_heap_top;

  uint32_t size = (&system_heap_top - &system_heap_base);

  if (0 != (size & 0xfff)) PANIC;

  memory_mapping map_system_heap = {
    .base_page = claim_contiguous_memory( size >> 12 ),
    .pages = size >> 12,
    .vap = &system_heap_base,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  map_memory( &map_system_heap );

  heap_initialise( &system_heap_base, size );
}

void setup_shared_heap()
{
  // Shared heap of memory that's user rwx
  extern uint8_t shared_heap_base;
  extern uint8_t shared_heap_top;

  uint32_t size = (&shared_heap_top - &shared_heap_base);

  if (0 != (size & 0xfff)) PANIC;

  memory_mapping map_shared_heap = {
    .base_page = claim_contiguous_memory( size >> 12 ),
    .pages = size >> 12,
    .vap = &shared_heap_base,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &map_shared_heap );

  heap_initialise( &shared_heap_base, size );
}

// If you connect a LED and roughly 1kOhm resistor in series to
// GPIO pins 22 and 27 (physical pins 15 and 13, respectively)
// down to ground (e.g. physical pin 1), this will alternately
// blink them.

static inline
void timer_set_countdown( int32_t timer )
{
  asm volatile ( "mcr p15, 0, %[t], c14, c2, 0" : : [t] "r" (timer) );
  // Clear interrupt and enable timer
  asm volatile ( "mcr p15, 0, %[config], c14, c2, 1" : : [config] "r" (1) );
}

void ticker( uint32_t handle, uint32_t core, uint32_t qa7_page )
{
  handle = handle; // Task handle not used

  QA7 volatile *qa7 = Task_MapDevicePages( 0x7000, qa7_page, 1 );

  qa7->timer_prescaler = 0x06AAAAAB;

  uint32_t clock_frequency;

  asm volatile ( "mrc p15, 0, %[clk], c14, c0, 0" : [clk] "=r" (clock_frequency) );

  uint32_t ticks_per_interval = clock_frequency / 1000; // milliseconds, honest!

#ifdef QEMU
  const int slower = 100;
  ticks_per_interval = ticks_per_interval * slower;
#endif

  Task_RegisterInterruptSources( 1 );

  ensure_changes_observable(); // About to write to QA7

  qa7->GPU_interrupts_routing = core;
  qa7->Core_IRQ_Source[core] = 0xffd;

  qa7->Core_timers_Interrupt_control[core] = 1;

  ensure_changes_observable(); // Wrote to QA7

  Task_EnablingIntterupt();

  timer_set_countdown( ticks_per_interval );

  for (;;) {
    Task_WaitForInterrupt( 0 );

    timer_set_countdown( ticks_per_interval );

    // There's only one place in each system that calls this,
    // so there's no point in putting it into ostaskops.h
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_Tick)
      : "lr", "cc" );
  }
}

void start_ticker( uint32_t qa7_page )
{
  // Writing the cp15 registers has to be done at svc32 (module init)
#ifdef QEMU
  const uint32_t clock_frequency = 62500000;
#else
  const uint32_t clock_frequency = 1000000; // Pi3 with default prescaler - 1MHz (checked manually over 60s)
#endif
  // For information only. CNTFRQ
  asm volatile ( "mcr p15, 0, %[clk], c14, c0, 0" : : [clk] "r" (clock_frequency) );

  // No event stream, EL0 accesses not trapped to undefined: CNTHCTL
  asm volatile ( "mcr p15, 0, %[config], c14, c1, 0" : : [config] "r" (0x303) );

  // End


  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = ticker;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = workspace.core;
  register uint32_t r2 asm( "r3" ) = qa7_page;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}

void blink_some_leds( uint32_t handle, uint32_t gpio_page )
{
  handle = handle;
  GPIO volatile *gpio = Task_MapDevicePages( 0x7000, gpio_page, 1 );

  uint32_t yellow = 27;
  uint32_t green = 22;

  set_state( gpio, yellow, GPIO_Output );
  set_state( gpio, green, GPIO_Output );
  push_writes_to_device();

  gpio->gpset[yellow/32] = 1 << (yellow % 32);
  gpio->gpset[green/32] = 1 << (green % 32);
  push_writes_to_device();

  for (;;) {
    Task_Sleep( 100 );
    // for (int i = 0; i < 1 << 24; i++) asm ( "nop" );
    gpio->gpset[yellow/32] = 1 << (yellow % 32);
    gpio->gpclr[green/32] = 1 << (green % 32);
    push_writes_to_device();
    Task_Sleep( 900 );
    // for (int i = 0; i < 1 << 25; i++) asm ( "nop" );
    gpio->gpclr[yellow/32] = 1 << (yellow % 32);
    gpio->gpset[green/32] = 1 << (green % 32);
    push_writes_to_device();
  }

  __builtin_unreachable();
}

void start_blink_some_leds( uint32_t gpio_page )
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = blink_some_leds;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = gpio_page;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void send_to_uart( uint32_t handle, uint32_t uart_page, uint32_t pipe )
{
  handle = handle;
  UART volatile *uart = Task_MapDevicePages( 0x7000, uart_page, 1 );
  uart->control = 0x31; // enable, tx & rx
  push_writes_to_device();
  uart->data = 'X';
  push_writes_to_device();

  PipeSpace data = {};
  for (;;) {
    if (data.available == 0)
      data = PipeOp_WaitForData( pipe, 16 );

asm ( "udf 88" );
    for (int i = 0; i < data.available; i++) {
      uart->data = ((char const*)data.location)[i];
      push_writes_to_device();
    }

    data = PipeOp_DataConsumed( pipe, data.available );
  }

  __builtin_unreachable();
}

void start_send_to_uart( uint32_t uart_page, uint32_t pipe )
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = send_to_uart;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = uart_page;
  register uint32_t r2 asm( "r3" ) = pipe;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}

void feed_pipe( uint32_t handle, uint32_t pipe )
{
  handle = handle;

  PipeSpace space = {};
  char c = 'C';
  for (;;) {
    Task_Sleep( 10 );
    if (space.available == 0)
      space = PipeOp_WaitForSpace( pipe, 1 );

    *(char*) space.location = c;
    c++;
    if (c == 'Z') c = 'A';
    space = PipeOp_SpaceFilled( pipe, 1 );
  }

  __builtin_unreachable();
}

void start_feed_pipe( uint32_t pipe )
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = feed_pipe;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = pipe;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}




void ping_pong( uint32_t handle, uint32_t to, uint32_t fro, bool i )
{
  handle = handle;

  PipeSpace space = {};
  PipeSpace data = {};

  for (;;) {
    if (space.available < 5)
      space = PipeOp_WaitForSpace( to, 5 );

    char *out = space.location;
    *out++ = 'P';
    *out++ = i ? 'i' : 'o';
    *out++ = 'n';
    *out++ = 'g';
    *out++ = ' ';

    space = PipeOp_SpaceFilled( to, 5 );

    if (data.available < 5)
      data = PipeOp_WaitForData( fro, 5 );

    char* in = data.location;

    if ((data.available != 5)
     || (in[0] != 'P')
     || (in[1] != (i ? 'o' : 'i'))
     || (in[2] != 'n')
     || (in[3] != 'g')
     || (in[4] != ' ')) asm ( "udf 2" );

    data = PipeOp_DataConsumed( fro, 5 );
  }

  __builtin_unreachable();
}

void execute_swi( svc_registers *regs, int number )
{
  PANIC;
}

void __attribute__(( noreturn )) startup()
{
  // Running with multi-tasking enabled. This routine gets called
  // just once.

  setup_system_heap();
  setup_shared_heap();

  uint32_t pipe = PipeOp_CreateForTransfer( 4096 );
  if (pipe == 0) PANIC;

  PipeOp_SetSender( pipe, 0 );
  PipeOp_SetReceiver( pipe, 0 );

  if (pipe == 0) PANIC;

  start_blink_some_leds( 0x3f200000 >> 12 );
  start_send_to_uart( 0x3f201000 >> 12, pipe );
  start_feed_pipe( pipe );

  start_ticker( 0x40000000 >> 12 );

  uint32_t to = PipeOp_CreateForTransfer( 4096 );
  if (to == 0) PANIC;

  PipeOp_SetSender( to, 0 );
  PipeOp_SetReceiver( to, 0 );

  uint32_t fro = PipeOp_CreateForTransfer( 4096 );
  if (fro == 0) PANIC;

  PipeOp_SetSender( fro, 0 );
  PipeOp_SetReceiver( fro, 0 );

{
  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = ping_pong;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = to;
  register uint32_t r2 asm( "r3" ) = fro;
  register uint32_t r3 asm( "r4" ) = 1;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    : "lr", "cc" );
}
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = ping_pong;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = fro;
  register uint32_t r2 asm( "r3" ) = to;
  register uint32_t r3 asm( "r4" ) = 0;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    : "lr", "cc" );
}

  asm ( "mov sp, %[reset_sp]"
    "\n  cpsie aif, #0x10"
    :
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  for (;;) { asm ( "mov r0, #10000\n  svc %[swi]" : : [swi] "i" (OSTask_Sleep) ); }
}

