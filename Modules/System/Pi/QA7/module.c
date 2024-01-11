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
  uint32_t gpu_task[72];
  struct qa7_irq_sources tasks[]; // One set per core
};

#define MODULE_CHUNK "0x1000"

// SWI 1000 - claim interrupt
//    IN: r0 = irq number
//    OUT: All registers preserved, or error indicating
//         * already claimed
//         * invalid number (sin bin?)
// Call from init, before starting to manipulate hardware

// SWI 1001 - Wait for interrupt
//    IN: r0 = irq number
//    OUT: All registers preserved, or error indicating 
//         * shutdown
//         * invalid number (sin bin?)
//         * other task waiting (sin bin?)
// Call with interrupts disabled (i.e. after calling Task_EnablingInterrupts).
// This call enables the interrupt in the interrupt controller and returns
// when one occurs.

// IRQ numbers 0-71 are GPU interrupts, 128-... are QA7 interrupts.

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

static inline
void release_irq_tasks( uint32_t active, uint32_t *irq_tasks )
{
  while (active != 0) {
    if (0 != (1 & active)) {
      uint32_t task = *irq_tasks;
      if (task == 0) asm ( "bkpt 6" );
      *irq_tasks = 0;
      Task_ReleaseTask( task, 0 );
    }
    irq_tasks++;
    active = active >> 1;
  }
}

void release_gpu_handlers( workspace *ws )
{
  // Bits 10, 11, 12, 13 & 14 are IRQ 7, 9, 10, 18 & 19
  static uint32_t const mapping1[] = {
    0,
    (1 << 7),
    (1 << 9),
    (1 << 9) | (1 << 7),
    (1 << 10),
    (1 << 10) | (1 << 7),
    (1 << 10) | (1 << 9),
    (1 << 10) | (1 << 9) | (1 << 7),
    (1 << 18),
    (1 << 18) | (1 << 7),
    (1 << 18) | (1 << 9),
    (1 << 18) | (1 << 9) | (1 << 7),
    (1 << 18) | (1 << 10),
    (1 << 18) | (1 << 10) | (1 << 7),
    (1 << 18) | (1 << 10) | (1 << 9),
    (1 << 18) | (1 << 10) | (1 << 9) | (1 << 7),
    (1 << 19),
    (1 << 19) | (1 << 7),
    (1 << 19) | (1 << 9),
    (1 << 19) | (1 << 9) | (1 << 7),
    (1 << 19) | (1 << 10),
    (1 << 19) | (1 << 10) | (1 << 7),
    (1 << 19) | (1 << 10) | (1 << 9),
    (1 << 19) | (1 << 10) | (1 << 9) | (1 << 7),
    (1 << 19) | (1 << 18),
    (1 << 19) | (1 << 18) | (1 << 7),
    (1 << 19) | (1 << 18) | (1 << 9),
    (1 << 19) | (1 << 18) | (1 << 9) | (1 << 7),
    (1 << 19) | (1 << 18) | (1 << 10),
    (1 << 19) | (1 << 18) | (1 << 10) | (1 << 7),
    (1 << 19) | (1 << 18) | (1 << 10) | (1 << 9),
    (1 << 19) | (1 << 18) | (1 << 10) | (1 << 9) | (1 << 7) };

  // Bits 15-19 & 20 are IRQs 21-25 & 30
  static uint32_t const mapping2[] = {
    0,
    (1 << 21),
    (1 << 22),
    (1 << 22) | (1 << 21),
    (1 << 23),
    (1 << 23) | (1 << 21),
    (1 << 23) | (1 << 22),
    (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 24),
    (1 << 24) | (1 << 21),
    (1 << 24) | (1 << 22),
    (1 << 24) | (1 << 22) | (1 << 21),
    (1 << 24) | (1 << 23),
    (1 << 24) | (1 << 23) | (1 << 21),
    (1 << 24) | (1 << 23) | (1 << 22),
    (1 << 24) | (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 25),
    (1 << 25) | (1 << 21),
    (1 << 25) | (1 << 22),
    (1 << 25) | (1 << 22) | (1 << 21),
    (1 << 25) | (1 << 23),
    (1 << 25) | (1 << 23) | (1 << 21),
    (1 << 25) | (1 << 23) | (1 << 22),
    (1 << 25) | (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 25) | (1 << 24),
    (1 << 25) | (1 << 24) | (1 << 21),
    (1 << 25) | (1 << 24) | (1 << 22),
    (1 << 25) | (1 << 24) | (1 << 22) | (1 << 21),
    (1 << 25) | (1 << 24) | (1 << 23),
    (1 << 25) | (1 << 24) | (1 << 23) | (1 << 21),
    (1 << 25) | (1 << 24) | (1 << 23) | (1 << 22),
    (1 << 25) | (1 << 24) | (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 30),
    (1 << 30) | (1 << 21),
    (1 << 30) | (1 << 22),
    (1 << 30) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 23),
    (1 << 30) | (1 << 23) | (1 << 21),
    (1 << 30) | (1 << 23) | (1 << 22),
    (1 << 30) | (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 24),
    (1 << 30) | (1 << 24) | (1 << 21),
    (1 << 30) | (1 << 24) | (1 << 22),
    (1 << 30) | (1 << 24) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 24) | (1 << 23),
    (1 << 30) | (1 << 24) | (1 << 23) | (1 << 21),
    (1 << 30) | (1 << 24) | (1 << 23) | (1 << 22),
    (1 << 30) | (1 << 24) | (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 25),
    (1 << 30) | (1 << 25) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 22),
    (1 << 30) | (1 << 25) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 23),
    (1 << 30) | (1 << 25) | (1 << 23) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 23) | (1 << 22),
    (1 << 30) | (1 << 25) | (1 << 23) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 24),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 22),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 22) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 23),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 23) | (1 << 21),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 23) | (1 << 22),
    (1 << 30) | (1 << 25) | (1 << 24) | (1 << 23) | (1 << 22) | (1 << 21)
  };

  // Assuming pending registers only include enabled IRQs
  uint32_t base_pending;

  base_pending = gpu->base_pending;

  uint32_t pending1;

  if (0 != (base_pending & (1 << 8))) {
    pending1 = gpu->pending1;
    // (Includes all the ones mapped to base_pending)
  }
  else {
    pending1 = mapping1[(base_pending >> 10) & 31];
  }

  if (pending1 != 0) {
    gpu->disable_irqs1 = pending1;
    release_irq_tasks( pending1, &ws->gpu_task[0] );
  }

  uint32_t pending2;

  if (0 != (base_pending & (1 << 9))) {
    pending2 = gpu->pending2;
  }
  else {
    pending2 = mapping2[(base_pending >> 15) & 63];
  }

  if (pending2 != 0) {
    gpu->disable_irqs2 = pending2;
    release_irq_tasks( pending2, &ws->gpu_task[32] );
  }

  base_pending &= 0xff;
  if (0 != base_pending) {
    gpu->disable_base = base_pending;
    release_irq_tasks( base_pending, &ws->gpu_task[64] );
  }

  ensure_changes_observable();
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
    Task_LogString( "Claiming GPU interrupts\n", 0 );

    // This core is claiming the GPU interrupts
    gpu->disable_irqs1 = 0xffffffff;
    gpu->disable_irqs2 = 0xffffffff;
    gpu->disable_base = 0xff;
    ensure_changes_observable();

    qa7->GPU_interrupts_routing = core;

    ensure_changes_observable();
  }

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

      release_gpu_handlers( ws );
    }

    release_irq_tasks( interrupts, core_irq_tasks );
  }
}

//////////////////////////////////////////////////

void ticker( uint32_t handle, QA7 volatile *qa7 )
{
  static const uint32_t irq_number = 139; // QA7 local timer

  Task_LogString( "Claiming QA7 timer interrupt\n", 0 );

  // This request may change the core we're running on...
  // This request used to do that, but I'll update the server to
  // switch to this core before releasing the client.
  register uint32_t irq asm ( "r0" ) = irq_number;
  asm ( "svc 0x1000" : : "r" (irq) );

  Task_LogString( "Claimed QA7 timer interrupt\n", 0 );

  Task_EnablingInterrupts();

  core_info cores = Task_Cores();
  int core = cores.current;

  qa7->Local_Interrupt_routing0 = core; // IRQ to this core, please

  // Just in case the above needs to be processed before the first
  // interrupt...
  ensure_changes_observable();

  // Automatically reloads when reaches zero
  // Interrupt cleared by writing (1 << 31) to Local_timer_write_flags
  qa7->Local_timer_control_and_status = 
      (1 << 29) | // Interrupt enable
      (1 << 28) | // Timer enable
#ifdef QEMU
      3840000; // 19.2 MHz clock, 38.4 MHz ticks, 1 ms * 100 for qemu
#else
      38400; // 19.2 MHz clock, 38.4 MHz ticks, 1 ms
#endif

  ensure_changes_observable(); // Wrote to QA7

  Task_LogString( "Waiting for first QA7 timer interrupt\n", 0 );

  for (;;) {
    register uint32_t irq asm ( "r0" ) = irq_number;
    asm ( "svc 0x1001" : : "r" (irq) );

    // Clear interrupt flag
    qa7->Local_timer_write_flags = (1 << 31);

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
  Task_LogString( "Starting ticker\n", 0 );

  static uint32_t const stack_size = 72;
  uint8_t *stack = rma_claim( stack_size );

  register void *start asm( "r0" ) = ticker;
  register uint32_t sp asm( "r1" ) = aligned_stack( stack + stack_size );
  register QA7 volatile *r1 asm( "r2" ) = qa7;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );

  Task_LogString( "Started ticker\n", 0 );
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
#else
#warning "Sleep ticker disabled"
#endif

  for (;;) {
    queued_task client = Task_QueueWait( ws->queue );

    switch (client.swi & 63) {
    case 0: // Claim interrupt
      // TODO: Set a flag.
      // The primary purpose of this call is to avoid tasks enabling
      // an interrupt before the interrupt controller is ready.

      // We could do without this if all interrupts could be masked, but
      // some have to be enabled or disabled at source (which we don't want
      // to know about).

      // It might also come in useful if we name interrupt sources etc.

      Task_SwitchToCore( client.core );
      Task_ReleaseTask( client.task_handle, 0 );
      break;
    case 1: // Wait for interrupt
      {
        svc_registers regs;

        Task_GetRegisters( client.task_handle, &regs );

        uint32_t req = regs.r[0];

        if (0 == (regs.spsr & 0x80)) asm ( "bkpt 2" );
        if ((req >= 72 && req < 128) || req >= 128 + 12) asm ( "bkpt 3" );

        uint32_t *task_entry;
        uint32_t handler;

        if (req < 72) { // GPU interrupt
          task_entry = &ws->gpu_task[req];
          handler = ws->gpu_handler;
        }
        else { // QA7 interrupt
          task_entry = &ws->tasks[client.core].task[req-128];
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
      break;
    default:
      asm ( "bkpt 3" );
    }
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
  handlers.action[1].queue = ws->queue;

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


