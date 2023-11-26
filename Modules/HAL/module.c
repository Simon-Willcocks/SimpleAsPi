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
#include "qa7.h"
#include "bcm_gpio.h"
#include "bcm_gpu.h"

typedef struct workspace workspace;

typedef struct qa7_irq_sources qa7_irq_sources;

struct qa7_irq_sources {
  uint32_t *tasks[12];
};

struct workspace {
  GPU *gpu;
  QA7 *qa7;
  uint32_t lock;
  struct qa7_irq_sources tasks[]; // One set per core
};

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

#include "ostaskops.h"

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

const char title[] = "HAL";
const char help[] = "RasPi3 HAL\t0.01";

static inline
uint64_t timer_now()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 0, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo) : : "memory"  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline
uint32_t timer_interrupt_time()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 2, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo)  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline
void timer_interrupt_at( uint64_t then )
{
  asm volatile ( "mcrr p15, 2, %[lo], %[hi], c14" : : [hi] "r" (then >> 32), [lo] "r" (0xffffffff & then) : "memory" );
}

static inline
void timer_set_countdown( int32_t timer )
{
  asm volatile ( "mcr p15, 0, %[t], c14, c2, 0" : : [t] "r" (timer) );
  // Clear interrupt and enable timer
  asm volatile ( "mcr p15, 0, %[config], c14, c2, 1" : : [config] "r" (1) );
}

static inline
int32_t timer_get_countdown()
{
  int32_t timer;
  asm volatile ( "mrc p15, 0, %[t], c14, c2, 0" : [t] "=r" (timer) );
  return timer;
}

static inline
uint32_t timer_status()
{
  uint32_t bits;
  asm volatile ( "mrc p15, 0, %[config], c14, c2, 1" : [config] "=r" (bits) );
  return bits;
}

static inline
bool timer_interrupt_active()
{
  return (timer_status() & 4) != 0;
}

void setup_clock_svc()
{
  const uint32_t clock_frequency = 1000000;

  // For information only. CNTFRQ
  asm volatile ( "mcr p15, 0, %[clk], c14, c0, 0"
                 :
                 : [clk] "r" (clock_frequency) );

  // No event stream, EL0 accesses not trapped to undefined: CNTHCTL
  asm volatile ( "mcr p15, 0, %[config], c14, c1, 0"
                 :
                 : [config] "r" (0x303) );
}

void irq_task( uint32_t handle, uint32_t core, workspace *ws )
{
  Task_LockClaim( &ws->lock, handle );
  Task_EnablingInterrupts();
  //ws->qa7->
  Task_LockRelease( &ws->lock );
  for (;;) {
    Task_WaitForInterrupt();
  }
}

void __attribute__(( noinline )) c_init( uint32_t core,
                                         workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  core_info cores = Task_Cores();

  if (*private == 0) {
    int size = sizeof( workspace ) + cores.total * sizeof( qa7_irq_sources );
    *private = rma_claim( size );
    memset( *private, 0, size );
  }
  else {
    asm ( "udf 1" );
  }

  workspace *ws = *private;

  ws->qa7 = Task_MapDeviceGlobal( 0x40000000 >> 12 );
  ws->gpu = Task_MapDeviceGlobal( 0x3f00b000 >> 12 );

  for (int i = 0; i < cores.total; i++) {
    Task_SwitchToCore( i );

    setup_clock_svc();

    uint32_t const stack_size = 256;
    uint8_t *stack = rma_claim( stack_size );

    register void *start asm( "r0" ) = irq_task;
    register void *sp asm( "r1" ) = stack + stack_size;
    register uint32_t r1 asm( "r2" ) = i;
    register workspace *r2 asm( "r3" ) = ws;
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_Create)
      , "r" (start)
      , "r" (sp)
      , "r" (r1)
      , "r" (r2)
      : "lr", "cc" );
  }
}

void __attribute__(( naked )) init( uint32_t core )
{
  register struct workspace **private asm ( "r12" );
  register char const *env asm ( "r10" );
  register uint32_t instantiation asm ( "r11" );

  // Move r12 into argument register
  asm volatile ( "push { lr }" );

  c_init( core, private, env, instantiation );

  asm ( "pop { pc }" );
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


  timer_set_countdown( ticks_per_interval );

  for (;;) {

    timer_set_countdown( ticks_per_interval );

    // There's only one place in each system that calls this,
    // so there's no point in putting it into ostaskops.h
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_Tick)
      : "lr", "cc" );
  }
}

void __attribute__(( noreturn )) boot( char const *cmd, workspace *ws )
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = rma_claim( stack_size );
  core_info cores = Task_Cores();
  uint32_t core = cores.current;

  register void *start asm( "r0" ) = ticker;
  register void *sp asm( "r1" ) = stack + stack_size;
  register uint32_t r1 asm( "r2" ) = core;
  register uint32_t r2 asm( "r3" ) = 0x40000000 >> 12;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );

  char command[64];
  char *mod;
  // -fPIE does insane stuff with strings, copying the rodata string
  // to the stack so that I can copy it onto the stack from the wrong
  // address...
  // I think the offset tables are supposed to be fixed up by the standard
  // library (which isn't going to work in ROM modules, is it?).
  // Compile ROM modules as fixed location, but check there's no .data segment
  // TODO
  asm ( "mov %[m], #0"
    "\n0:"
    "\n  ldrb lr, [%[s], %[m]]"
    "\n  cmp lr, #0"
    "\n  strb lr, [%[d], %[m]]"
    "\n  addne %[m], %[m], #1"
    "\n  bne 0b"
    "\n  add %[m], %[d], %[m]"
    : [m] "=&r" (mod)
    : [s] "r" ("System:Modules.")
    , [d] "r" (command)
    : "lr" );

  char const *s =
    "BCM283XGPIO\0"
/*
    "BCM283XGPU\0"
    "FileSwitch\0"
    "ResourceFS\0"
    "TerritoryManager\0"
    "Messages\0"
    "MessageTrans\0"
    "UK\0"
    "WindowManager\0"
    "TaskManager\0"
    "Desktop\0"
    "SharedRISC_OSLib\0"
    "BASIC105\0"
    "BASIC64\0"
    "BASICVFP\0"
    "BlendTable\0"
    "BufferManager\0"
    "ColourTrans\0"
    "DeviceFS\0"
*/
; // End of string!

  while (*s != '\0') {
    char *p = mod;
    do {
      *p++ = *s++;
    } while (*s != '\0');
    *p = '\0';
    s++;

    register uint32_t load asm ( "r0" ) = 1; // RMLoad
    register char const *module asm ( "r1" ) = command;
    register error_block *error asm ( "r0" );

    asm ( "svc %[swi]"
      "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OS_Module), "r" (load), "r" (module) );
    if (error != 0) {
      asm ( "udf 7" );
    }
  }

  for (;;) { Task_Sleep( 100000 ); asm ( "udf 8" ); }

  __builtin_unreachable();
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

void __attribute__(( naked )) start( char const *command )
{
  register workspace *ws asm ( "r12" );

  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  boot( command, ws );

  __builtin_unreachable();
}

