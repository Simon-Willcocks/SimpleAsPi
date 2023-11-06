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
#include "heap.h"

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

void Sleep( uint32_t ms )
{
  register uint32_t t asm( "r0" ) = ms;
  asm ( "svc %[swi]" : : [swi] "i" (OSTask_Sleep), "r" (t) );
}

static inline
void timer_set_countdown( int32_t timer )
{
  asm volatile ( "mcr p15, 0, %[t], c14, c2, 0" : : [t] "r" (timer) );
  // Clear interrupt and enable timer
  asm volatile ( "mcr p15, 0, %[config], c14, c2, 1" : : [config] "r" (1) );
}

void ticker( uint32_t handle, uint32_t core, QA7 *qa7, GPIO *gpio )
{
  handle = handle; // Task handle not used

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

  qa7->Core_timers_Interrupt_control[core] = 15;

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

void start_ticker( QA7 *qa7, GPIO *gpio )
{
  // Writing the cp15 registers has to be done at svc32
  qa7->timer_prescaler = 0x06AAAAAB;
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
  register uint32_t r0 asm( "r2" ) = workspace.core;
  register void *r1 asm( "r3" ) = qa7;
  register void *r2 asm( "r4" ) = gpio;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r0)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}

void blink_some_leds( uint32_t handle, GPIO volatile *gpio )
{
  handle = handle;

  uint32_t yellow = 27;
  uint32_t green = 22;

  set_state( gpio, yellow, GPIO_Output );
  set_state( gpio, green, GPIO_Output );
  push_writes_to_device();

  gpio->gpset[yellow/32] = 1 << (yellow % 32);
  gpio->gpset[green/32] = 1 << (green % 32);
  push_writes_to_device();

  for (;;) {
    // for (int i = 0; i < 1 << 24; i++) asm ( "nop" );
    gpio->gpset[yellow/32] = 1 << (yellow % 32);
    gpio->gpclr[green/32] = 1 << (green % 32);
    push_writes_to_device();
    Sleep( 50 );
    // for (int i = 0; i < 1 << 25; i++) asm ( "nop" );
    gpio->gpclr[yellow/32] = 1 << (yellow % 32);
    gpio->gpset[green/32] = 1 << (green % 32);
    push_writes_to_device();
    Sleep( 100 );
  }

  __builtin_unreachable();
}

void start_blink_some_leds( GPIO *gpio )
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = shared_heap_allocate( stack_size );

  register void *start asm( "r0" ) = blink_some_leds;
  register void *sp asm( "r1" ) = stack + stack_size;
  register void *r0 asm( "r2" ) = gpio;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r0)
    : "lr", "cc" );
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
    .usr32_access = 1 };
  map_memory( &map_qa7 );

  GPIO *gpio = (void*) 0xfff01000;

  memory_mapping map_gpio = {
    .base_page = 0x3f200000 >> 12,
    .pages = 1,
    .vap = gpio,
    .type = CK_Device,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 1 };
  map_memory( &map_gpio );

  setup_system_heap();
  setup_shared_heap();

  start_blink_some_leds( gpio );

  start_ticker( qa7, gpio );

  asm ( "mov sp, %[reset_sp]"
    "\n  cpsid aif, #0x10"
    :
    : [reset_sp] "r" ((&workspace.svc_stack)+1) );

  for (;;) { asm ( "mov r0, #10000\n  svc %[swi]" : : [swi] "i" (OSTask_Sleep) ); }
}

