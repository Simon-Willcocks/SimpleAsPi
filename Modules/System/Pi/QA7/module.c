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
#include "qa7.h"

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
  struct qa7_irq_sources tasks[]; // One set per core
};

#define MODULE_CHUNK "0x1000"

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

QA7 volatile *const qa7 = (void*) 0x7000;

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

  for (;;) {
    Task_WaitForInterrupt();

    uint32_t interrupts = qa7->Core_IRQ_Source[core];
    uint32_t *irq_task = core_irq_tasks;

// This approach is multi processor safe because:
//  Word read/writes are atomic.
//  This function will read the irq task handle once only (per loop)
//  This function will only set the handle to zero if non-zero was read
//  The function writing the handles only reads the word once (per wait)
//  The function writing the handles will only write a non-zero value if
// zero was read

    while (interrupts != 0) {
      if (0 != (1 & interrupts)) {
        if (*irq_task == 0) asm ( "bkpt 6" );
        Task_ReleaseTask( *irq_task, 0 );
        *irq_task = 0;
      }
      irq_task++;
      interrupts = interrupts >> 1;
    }
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

  qa7->GPU_interrupts_routing = core;
  qa7->Core_IRQ_Source[core] = 0xffd;

  qa7->Core_timers_Interrupt_control[core] = 1;

  ensure_changes_observable(); // Wrote to QA7

  Task_EnablingInterrupts();

  timer_set_countdown( ticks_per_interval );

  for (;;) {
    register uint32_t request asm ( "r0" ) = 0;
    asm ( "svc 0x1000" : : "r" (request) );

    timer_set_countdown( ticks_per_interval );

    // There's only one place in each system that calls this,
    // so there's no point in putting it into ostaskops.h
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_Tick)
      : "lr", "cc" );
    Task_LogString( "Tick", 4 );
  }
}

void start_ticker()
{
  static uint32_t const stack_size = 72;
  uint8_t *stack = rma_claim( stack_size );
  core_info cores = Task_Cores();
  uint32_t core = cores.current;

  register void *start asm( "r0" ) = ticker;
  register void *sp asm( "r1" ) = stack + stack_size;
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

void irq_manager( uint32_t handle, workspace *ws )
{
  uint32_t qa7_page = 0x40000000 >> 12;
  Task_MapDevicePages( (uint32_t) qa7, qa7_page, 1 );

  // One IRQ task per core
#ifdef DEBUG__SINGLE_CORE
  {
    int i = 0;
#else
  for (int i = 0; i < ws->cores.total; i++) {
#endif

    uint32_t const stack_size = 256;
    uint8_t *stack = rma_claim( stack_size );

    // Note: The above rma_claim may (probably will) result on the core
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
    register void *sp asm( "r1" ) = stack + stack_size;
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
    // It's not worth defining the whole worspace as volatile as
    // very little shared stuff gets changed.
    uint32_t volatile *h = &ws->tasks[i].core_irq_task;
    while (0 == *h) {
      Task_Yield();
    }

    if (handle != ws->tasks[i].core_irq_task) asm ( "bkpt 10" );
  }

  Task_EnablingInterrupts();

  start_ticker();

  for (;;) {
    queued_task client = Task_QueueWait( ws->queue );

    svc_registers regs;

    Task_GetRegisters( client.task_handle, &regs );

    uint32_t req = regs.r[0];

    if (0 == (regs.spsr & 0x80)) asm ( "bkpt 2" );
    if (req >= 12) asm ( "bkpt 3" );
    if (0 != ws->tasks[client.core].task[req]) asm ( "bkpt 4" );
    if (8 == req) {
      // Only ever one core receiving GPU interrupts...
      for (int i = 0; i < ws->cores.total; i++) {
        if (0 != ws->tasks[i].task[8]) asm ( "bkpt 5" );
      }
    }

    // Ensure the irq task has the right to release the client
    // before telling it about the client.
    uint32_t handler = ws->tasks[client.core].core_irq_task;

    if (handler == 0) asm ( "bkpt 7" : : "r" (client.core | 0x44440000) );

    Task_ChangeController( client.task_handle, handler );

    ws->tasks[client.core].task[req] = client.task_handle;
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
  register void *sp asm( "r1" ) = stack + stack_size;
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


