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
#include "legacy.h"
#include "kernel_swis.h"

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

void svc_pre_boot_sequence()
{
  //start_ticker( 0x40000000 >> 12 );
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

void boot()
{
  start_blink_some_leds( 0x3f200000 >> 12 );

  if (0) {
  register uint32_t base asm ( "r0" ) = 10;
  register char const *string asm ( "r1" ) = "77";
  register uint32_t value asm ( "r2" );
  asm ( "svc %[swi]" : "=r" (value) : [swi] "i" (OS_ReadUnsigned), "r" (base), "r" (string) );
  if (value != 77) PANIC;
  }

  // RMRun HAL
  register uint32_t run asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules.HAL";

  asm ( "svc %[swi]" : : [swi] "i" (OS_Module), "r" (run), "r" (module) );

  PANIC;

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) boot_sequence()
{
  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0", "r1" );

  boot();
}
