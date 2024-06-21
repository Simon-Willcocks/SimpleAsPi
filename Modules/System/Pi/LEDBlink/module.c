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

// QA7 module, for use on Raspberry Pi 2 - 3

#include "CK_types.h"
#include "ostaskops.h"
#include "bcm_gpio.h"

typedef struct workspace workspace;

#define MODULE_CHUNK "0x1040"

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

const char title[] = "LEDBlink";
const char help[] = "BCM Blink LED\t0.01 (" CREATION_DATE ")";

// For consistency, this should be 7000 (work our way down from 0x8000,
// a page at a time), but for debugging...
GPIO volatile *const gpio = (void*) 0x6000;

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

static inline void led_on( uint32_t pin )
{
  gpio->gpset[pin / 32] = 1 << (pin & 31);
  push_writes_to_device();
}

static inline void led_off( uint32_t pin )
{
  gpio->gpclr[pin / 32] = 1 << (pin & 31);
  push_writes_to_device();
}

void blinker( uint32_t handle, uint32_t pin,
              uint32_t on_time, uint32_t off_time )
{
#if 0
  Task_LogString( "Printing a * on screen\n", 0 );

  asm ( "svc 0x11a" ); // Reset text and graphics windows

  for (int i = 0; i < 10; i++)
  asm ( "svc 0x12a" ); // Printable character '*'

  Task_MemoryChanged( 0xc0000000, 8 << 20 );



uint32_t *screen = (void*) 0xc0000000;
for (int y = 100; y < 200; y++) {
  for (int x = 200; x < 400; x++) {
    screen[x + 1920 * y] = 0x4488cc22;
  }
}
  Task_MemoryChanged( 0xc0000000, 8 << 20 );

  Task_LogString( "Drawing a rectangle on screen\n", 0 );

  asm ( "svc 0x119" // VDU 25 (OS_Plot)
    "\n  svc 0x104"
    "\n  svc 0x160"
    "\n  svc 0x107"
    "\n  svc 0x1e8"
    "\n  svc 0x103"
    "\n  svc 0x165" // Rect, filled 32,32 - 1888,1000
    "\n  svc 0x120"
    "\n  svc 0x100"
    "\n  svc 0x120"
    "\n  svc 0x100" );

  register char const *str asm( "r0" ) = "Hello world";
  asm ( "svc 2" : : "r" (str) : "lr" ); // Write0
#endif
  for (;;) {
    // Task_LogString( "ON ", 0 ); Task_LogSmallNumber( pin ); Task_LogNewLine();
    led_on( pin );
    Task_Sleep( on_time );
    // Task_LogString( "OFF ", 0 ); Task_LogSmallNumber( pin ); Task_LogNewLine();
    led_off( pin );
    Task_Sleep( off_time );
    asm ( "swi 0x12a" );
  }
}

void start_blinker( svc_registers *regs )
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = rma_claim( stack_size );
  uint32_t pin = regs->r[0];
  uint32_t on = regs->r[1];
  uint32_t off = regs->r[2];

  set_state( gpio, pin, GPIO_Output );
  push_writes_to_device();

  register void *start asm( "r0" ) = blinker;
  register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
  register uint32_t r1 asm( "r2" ) = pin;
  register uint32_t r2 asm( "r3" ) = on;
  register uint32_t r3 asm( "r4" ) = off;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    : "lr", "cc" );
}

//////////////////////////////////////////////////

void led_manager( uint32_t handle, uint32_t queue )
{
  Task_LogString( "led_manager", 0 );

  uint32_t gpio_page = 0x3f200000 >> 12;
  Task_MapDevicePages( gpio, gpio_page, 1 );

  set_state( gpio, 27, GPIO_Output );
  push_writes_to_device();
  led_on( 27 ); // Yellow
  Task_LogString( "Yellow off\n", 0 );

  for (;;) {
    queued_task client = Task_QueueWait( queue );

    svc_registers regs;

    Task_GetRegisters( client.task_handle, &regs );

    Task_LogString( "Start Blinking\n", 0 );
    start_blinker( &regs );
    Task_LogString( "Started Blinking\n", 0 );

    Task_ReleaseTask( client.task_handle, 0 );
  }
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  uint32_t queue = Task_QueueCreate();
  swi_handlers handlers = { };
  handlers.action[0].queue = queue;

  Task_RegisterSWIHandlers( &handlers );

  uint32_t const stack_size = 256;
  uint8_t *stack = rma_claim( stack_size );

  register void *start asm( "r0" ) = led_manager;
  register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
  register uint32_t r1 asm( "r2" ) = queue;
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

void go()
{
  Task_LogString( "Entering blink module\n", 0 );
  if (1) {
  register uint32_t pin asm( "r0" ) = 22; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 200;
  register uint32_t off asm( "r2" ) = 100;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );
  }

  if (1) {
  register uint32_t pin asm( "r0" ) = 27; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 95;
  register uint32_t off asm( "r2" ) = 55;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );
  }

  for (;;) {
    for (int i = 0;i < 100; i++) {
      Task_Sleep( 50 );
      Task_LogString( "Time has passed\n", 0 );
    }
    Task_Sleep( 500 );
    Task_LogString( "A lot of time has passed\n", 0 );
  }
  for (;;) {
    asm ( "bkpt 1" );
  }
}

void start()
{
  // Running in usr32 mode, no stack
  // led_manager may not yet have started
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  go();
}
