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

const char title[] = "LEGBlink";
const char help[] = "BCM Blink LED\t0.01 (" CREATION_DATE ")";

GPIO volatile *const gpio = (void*) 0x7000;

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
  for (;;) {
    led_on( pin );
    Task_Sleep( on_time );
    led_off( pin );
    Task_Sleep( off_time );
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
  register void *sp asm( "r1" ) = stack + stack_size;
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
  uint32_t gpio_page = 0x3f200000 >> 12;
  Task_MapDevicePages( (uint32_t) gpio, gpio_page, 1 );

  set_state( gpio, 22, GPIO_Output );
  push_writes_to_device();
  led_on( 22 ); // Green
  for (;;) {
    queued_task client = Task_QueueWait( queue );

    svc_registers regs;

    Task_GetRegisters( client.task_handle, &regs );

    start_blinker( &regs );

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
  register void *sp asm( "r1" ) = stack + stack_size;
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

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}


