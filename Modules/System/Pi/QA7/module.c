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
// Includes GPU interrupts

#include "CK_types.h"
#include "ostaskops.h"
#include "qa7.h"
#include "bcm_gpu.h"

typedef struct workspace workspace;

typedef struct qa7_irq_sources qa7_irq_sources;

struct qa7_irq_sources {
  uint32_t core_irq_task;
  uint32_t task[12];
};

struct workspace {
  uint32_t lock;
  uint32_t queue;
  core_info cores;
  struct {
    uint32_t stack[64];
  } runstack;
  uint32_t gpu_handler;
  uint32_t gpu_task[64];
  struct qa7_irq_sources tasks[]; // One set per core
};

#define MODULE_CHUNK "0x1000"

// SWI 1000 - Wait for interrupt r0 = irq number
// SWI 1001 - Wait for core interrupt r0 = irq number, r1 = core number
// IRQ numbers 0-63 are GPU interrupts, 64-76 are QA7 interrupts or
// something else! TODO

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

const char title[] = "QA7";
const char help[] = "BCM QA7\t\t0.01 (" CREATION_DATE ")";

static QA7 volatile *const qa7 = (void*) 0x7000;
static GPU volatile *const gpu = (void*) 0x6000;

// Multi-processing primitives. No awareness of OSTasks.

// Change the word at `word' to the value `to' if it contained `from'.
// Returns the original content of word (== from, if changed successfully)
static inline
uint32_t change_word_if_equal( uint32_t *word, uint32_t from, uint32_t to )
{
  uint32_t failed = true;
  uint32_t value;

  do {
    asm volatile ( "ldrex %[value], [%[word]]"
                   : [value] "=&r" (value)
                   : [word] "r" (word) );

    if (value == from) {
      // Assembler note:
      // The failed and word registers are not allowed to be the same, so
      // pretend to gcc that the word may be written as well as read.

      asm volatile ( "strex %[failed], %[value], [%[word]]"
                     : [failed] "=&r" (failed)
                     , [word] "+r" (word)
                     : [value] "r" (to) );

      ensure_changes_observable();
    }
    else {
      asm ( "clrex" );
      break;
    }
  } while (failed);

  return value;
}

void setup_clock_svc()
{
  const uint32_t clock_frequency = 1000000;

  // Allow EL0 to access the timer.
  // It would be better to use just qa7 access, but QEMU doesn't
  // seem to allow that at the moment...
  // For information only. CNTFRQ
  asm volatile ( "mcr p15, 0, %[clk], c14, c0, 0"
                 :
                 : [clk] "r" (clock_frequency) );

  // No event stream, EL0 accesses not trapped to undefined: CNTHCTL
  asm volatile ( "mcr p15, 0, %[config], c14, c1, 0"
                 :
                 : [config] "r" (0x303) );
}

static inline
void release_irq_tasks( uint32_t active, uint32_t *irq_tasks )
{
  while (active != 0) {
    if (0 != (1 & active)) {
      if (*irq_tasks == 0) asm ( "bkpt 6" );
      Task_ReleaseTask( *irq_tasks, 0 );
      *irq_tasks = 0;
    }
    irq_tasks++;
    active = active >> 1;
  }
}

void core_irq_task( uint32_t handle, uint32_t core, workspace *ws )
{
  core_info cores = Task_Cores();
  if (core != cores.current) asm ( "bkpt 11" );

  uint32_t *core_irq_tasks = &ws->tasks[core].task[0];

  Task_EnablingInterrupts();

  // Release the creating task to start generating interrupts on this
  // core. Even if that task is running on a different core, this core
  // is running with interrupts disabled, so there's no race condition.
  ws->tasks[core].core_irq_task = handle;

  if (0 == change_word_if_equal( &ws->gpu_handler, 0, handle )) {
    // This core is claiming the GPU interrupts
    gpu->disable_irqs1 = 0xffffffff;
    gpu->disable_irqs2 = 0xffffffff;
    gpu->disable_base = 0xff;
    ensure_changes_observable();

    // Writes to QA7
    // This core only
    qa7->GPU_interrupts_routing = core;
  }

  // All cores (do not spit up the qa7 writes)
  qa7->Core_IRQ_Source[core] = 0;
  ensure_changes_observable();

  for (;;) {
    Task_WaitForInterrupt();

    uint32_t interrupts = qa7->Core_IRQ_Source[core];

// This approach is multi processor safe because:
//  Word read/writes are atomic.
//  This function will read the irq task handle once only (per loop)
//  This function will only set the handle to zero if non-zero was read
//  The function writing the handles only reads the word once (per wait)
//  The function writing the handles will only write a non-zero value if
// zero was read

    if ((interrupts & (1 << 8)) != 0) { // GPU interrupt

      if (handle != ws->gpu_handler) asm ( "bkpt 8" );

      interrupts &= ~(1 << 8);
      // Assuming pending registers only include enabled IRQs
      uint32_t pending;

      pending = gpu->pending1;
      if (pending != 0) {
asm ( "udf 1" );
        gpu->disable_irqs1 = pending;
        release_irq_tasks( pending, &ws->gpu_task[0] );
      }

      pending = gpu->pending2;
      if (pending != 0) {
asm ( "udf 2" );
        gpu->disable_irqs2 = pending;
        release_irq_tasks( pending, &ws->gpu_task[32] );
      }

      if (interrupts != 0) {
        gpu->disable_base = interrupts & 0xff;
asm ( "udf 3" );
      }

      ensure_changes_observable();
    }

    release_irq_tasks( interrupts, core_irq_tasks );
  }
}

//////////////////////////////////////////////////

// This section will be moved to a separate module asap
// Two separate modules, actually, one for QEMU, the other for
// real hardware that implments the QA7 timer with automatic
// re-load.

static inline
void timer_set_countdown( int32_t timer )
{
  asm volatile ( "mcr p15, 0, %[t], c14, c2, 0" : : [t] "r" (timer) );
  // Clear interrupt and enable timer
  asm volatile ( "mcr p15, 0, %[config], c14, c2, 1" : : [config] "r" (1) );
}

void ticker( uint32_t handle, uint32_t core, QA7 volatile *qa7 )
{
  core_info cores = Task_Cores();
  if (core != cores.current) asm ( "bkpt 11" );

  qa7->timer_prescaler = 0x06aaaaab;

  uint32_t clock_frequency;

  asm volatile ( "mrc p15, 0, %[clk], c14, c0, 0" : [clk] "=r" (clock_frequency) );

  uint32_t ticks_per_interval = clock_frequency / 1000; // milliseconds, honest!

#ifdef QEMU
  const int slower = 100;
  ticks_per_interval = ticks_per_interval * slower;
#endif

  // Is this interrupt enable for the other 12 interrupts?
  // Documentations says it's status, not control.
  qa7->Core_IRQ_Source[core] = 0xfff;

  Task_EnablingInterrupts();

  qa7->Core_timers_Interrupt_control[core] = 1;

  ensure_changes_observable(); // Wrote to QA7

  timer_set_countdown( ticks_per_interval );

  for (;;) {
    register uint32_t request asm ( "r0" ) = 64;
    asm ( "svc 0x1000" : : "r" (request) );

    timer_set_countdown( ticks_per_interval );

    // There's only one place in each system that calls this,
    // so there's no point in putting it into ostaskops.h
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_Tick)
      : "lr", "cc" );
  }
}

void start_ticker()
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = rma_claim( stack_size );
  core_info cores = Task_Cores();
  uint32_t core = cores.current;

  register void *start asm( "r0" ) = ticker;
  register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
  register uint32_t r1 asm( "r2" ) = core;
  register QA7 volatile *r2 asm( "r3" ) = qa7;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    , "r" (r2)
    : "lr", "cc" );
}

//////////////////////////////////////////////////

#include "bcm_gpio.h"

GPIO volatile *const gpio = (void*) 0x2000;

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

void leds( int pin )
{
return;
  for (int n = 0; n < 10; n++) {
    led_off( pin );
    for (int i = 0; i < 0x1000000; i++) asm ( "" );
    led_on( pin );
    for (int i = 0; i < 0x1000000; i++) asm ( "" );
  }
}

void irq_manager( uint32_t handle, workspace *ws )
{
  uint32_t qa7_page = 0x40000000 >> 12;
  uint32_t const gpu_page = 0x3f00b000 >> 12;

  Task_MapDevicePages( qa7, qa7_page, 1 );
  Task_MapDevicePages( gpu, gpu_page, 1 );

  // EXPERIMENT:
  gpu->enable_base = 0xff;
  ensure_changes_observable(); // Wrote to GPU
  for (int core = 0; core < ws->cores.total; core++) {
    qa7->Core_IRQ_Source[core] = 0xfff;
    qa7->Core_timers_Interrupt_control[core] = 1;
  }
  ensure_changes_observable(); // Wrote to QA7
  // end. Not it!

  Task_MapDevicePages( gpio, 0x3f200, 1 );

  set_state( gpio, 22, GPIO_Output );
  set_state( gpio, 27, GPIO_Output );
  push_writes_to_device();
leds( 27 );

  // One IRQ task per core
  for (int i = 0; i < ws->cores.total; i++) {
    uint32_t const stack_size = 256;
    uint8_t *stack = rma_claim( stack_size );

    // Note: The above rma_claim may (probably will) result in the core
    // we're running on to change.
    // Therefore we have to run it before switching (or switch more than
    // once).

    Task_SwitchToCore( i );

    register void (*code)() asm ( "r0" ) = setup_clock_svc;
    register uint32_t out asm ( "r0" );

    asm volatile ( // volatile because I ignore the output value
          "svc %[swi]"
      : "=r" (out)
      : [swi] "i" (OSTask_RunPrivileged)
      , "r" (code) );

    register void *start asm( "r0" ) = core_irq_task;
    register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
    register uint32_t r1 asm( "r2" ) = i;
    register workspace *r2 asm( "r3" ) = ws;

    register uint32_t handle asm( "r0" );

    asm volatile ( "svc %[swi]" // volatile in case we ignore output
      : "=r" (handle)
      : [swi] "i" (OSTask_Create)
      , "r" (start)
      , "r" (sp)
      , "r" (r1)
      , "r" (r2)
      : "lr", "cc" );

    // Wait for the task to be ready to accept interrupts
    // Without the volatile keyword this loop gets optimised to
    // if (0 == ws->tasks[i].core_irq_task)
    //   for (;;) Task_Yield();
    // It's not worth defining the whole workspace as volatile as
    // very little shared stuff gets changed.

    uint32_t volatile *h = &ws->tasks[i].core_irq_task;
    while (0 == *h) {
      Task_Yield();
    }
    // Note: Yield means this core could be running on any core now.

    if (handle != ws->tasks[i].core_irq_task) asm ( "bkpt 10" );
  }

leds( 22 );
  Task_EnablingInterrupts();

#ifndef DEBUG__NO_TICKER
  start_ticker();
#endif

  for (;;) {
    queued_task client = Task_QueueWait( ws->queue );

    svc_registers regs;

    Task_GetRegisters( client.task_handle, &regs );

    uint32_t req = regs.r[0];

    if (0 == (regs.spsr & 0x80)) asm ( "bkpt 2" );
    if (req >= 72) asm ( "bkpt 3" );

    uint32_t *task_entry;
    uint32_t handler;

    if (req < 64) { // GPU interrupt
      task_entry = &ws->gpu_task[req];
      handler = ws->gpu_handler;
    }
    else { // QA7 interrupt
      task_entry = &ws->tasks[client.core].task[req-64];
      handler = ws->tasks[client.core].core_irq_task;
    }

    if (0 != *task_entry) asm ( "bkpt 4" );
    if (handler == 0) asm ( "bkpt 7" : : "r" (client.core | 0x47474747) );

    // Ensure the irq task has the right to release the client
    // before telling it about the client.

    Task_ChangeController( client.task_handle, handler );

    *task_entry = client.task_handle;

    ensure_changes_observable();

    // Allow the interrupt through to the ARM.
    if (req < 32) { // GPU interrupt, word 1
      gpu->enable_irqs1 = (1 << req);
    }
    else if (req < 64) { // GPU interrupt, word 1
      gpu->enable_irqs2 = (1 << (req - 32));
    }
    else if (req < 72) { // GPU interrupt, base
      gpu->enable_base = (1 << (req - 64));
    }

    ensure_changes_observable();
  }
}

void __attribute__(( noinline )) c_init( workspace **private,
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

  ws->cores = cores;

  ws->queue = Task_QueueCreate();
  swi_handlers handlers = { };
  handlers.action[0].queue = ws->queue;

  Task_RegisterSWIHandlers( &handlers );

  uint32_t const stack_size = 256;
  uint8_t *stack = rma_claim( stack_size );

  register void *start asm( "r0" ) = irq_manager;
  register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
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


