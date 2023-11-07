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

#include "ostask.h"
#include "mpsafe_dll.h"
#include "heap.h"

typedef struct OSTaskSlot OSTaskSlot;
typedef struct OSTask OSTask;

static inline uint32_t ostask_handle( OSTask *task )
{
  return 0x4b534154 ^ (uint32_t) task;
}

static inline OSTask *ostask_from_handle( uint32_t h )
{
  return (OSTask *) (0x4b534154 ^ h);
}

struct OSTaskSlot {
  uint32_t mmu_map;
  OSTaskSlot_extras extras;
  struct {
    uint32_t base;      // Pages
    uint32_t pages;     // Pages
    uint32_t va;        // Absolute
  } app_mem[30];

  // List is only used for free pool, ATM.
  OSTaskSlot *next;
  OSTaskSlot *prev;
};

struct __attribute__(( packed, aligned( 4 ) )) OSTask {
  svc_registers regs;
  uint32_t banked_sp_usr; // Only stored when leaving usr or sys mode
  uint32_t banked_lr_usr; // Only stored when leaving usr or sys mode
  int32_t resumes; // Signed: -1 => blocked
  OSTaskSlot *slot;

  OSTask_extras extras;

  OSTask *next;
  OSTask *prev;
};

// OK, to start with, temporarily, 1MiB sections
extern OSTask OSTask_free_pool[];
extern OSTaskSlot OSTaskSlot_free_pool[];

MPSAFE_DLL_TYPE( OSTask );
MPSAFE_DLL_TYPE( OSTaskSlot );

void setup_pools()
{
  memory_mapping tasks = {
    .base_page = claim_contiguous_memory( 0x100 ), // 1 MiB
    .pages = 0x100,
    .vap = &OSTask_free_pool,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (tasks.base_page == 0xffffffff) PANIC;
  map_memory( &tasks );
  // memset( &OSTask_free_pool, 0, 0x100000 );
  for (int i = 0; i < 100; i++) { // FIXME How many in a MiB?
    OSTask *t = &OSTask_free_pool[i];
    dll_new_OSTask( t );
    dll_attach_OSTask( t, &shared.ostask.task_pool );
    shared.ostask.task_pool = shared.ostask.task_pool->next;
  }

  memory_mapping slots = {
    .base_page = claim_contiguous_memory( 0x100 ), // 1 MiB
    .pages = 0x100,
    .vap = &OSTaskSlot_free_pool,
    .type = CK_MemoryRW,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (slots.base_page == 0xffffffff) PANIC;
  map_memory( &slots );
  // memset( &OSTaskSlot_free_pool, 0, 0x100000 );
  for (int i = 0; i < 100; i++) { // FIXME How many in a MiB?
    OSTaskSlot *s = &OSTaskSlot_free_pool[i];
    dll_new_OSTaskSlot( s );
    s->mmu_map = i; // FIXME: Will run out of values at 65536
    dll_attach_OSTaskSlot( s, &shared.ostask.slot_pool );
    shared.ostask.slot_pool = shared.ostask.slot_pool->next;
  }
}

static void setup_processor_vectors();

void __attribute__(( naked, noreturn )) idle_task();

void __attribute__(( noreturn )) boot_with_stack( uint32_t core )
{
  forget_boot_low_memory_mapping();

  bool reclaimed = core_claim_lock( &shared.ostask.lock, core + 1 );
  if (reclaimed) PANIC;

  bool first = (shared.ostask.first == 0);

  workspace.core = core;

  if (first)
  {
    extern uint8_t top_of_boot_RAM;
    extern uint8_t top_of_minimum_RAM;

    // Some RAM to be going on with...
    free_contiguous_memory( ((uint32_t) &top_of_boot_RAM) >> 12,
                            (&top_of_minimum_RAM - &top_of_boot_RAM) >> 12 );

    setup_pools();

    shared.ostask.first = mpsafe_detach_OSTaskSlot_at_head( &shared.ostask.slot_pool );
  }

  setup_processor_vectors();

  core_release_lock( &shared.ostask.lock );

  // Make this running code into an OSTask
  workspace.ostask.running = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );
  workspace.ostask.running->slot = shared.ostask.first;

  if (first) {
    // Create a separate idle OSTask
    workspace.ostask.idle = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );
    workspace.ostask.idle->regs.lr = (uint32_t) idle_task;

    uint32_t cpsr;
    asm ( "mrs %[cpsr], cpsr" : [cpsr] "=r" (cpsr) );

    // usr32, interrupts enabled
    workspace.ostask.idle->regs.spsr = cpsr & ~0x1ef;
    workspace.ostask.idle->slot = shared.ostask.first;

    dll_attach_OSTask( workspace.ostask.idle, &workspace.ostask.running );
    workspace.ostask.running = workspace.ostask.running->next;

    startup();
  }
  else {
    // Become the idle OSTask
    workspace.ostask.idle = workspace.ostask.running;
    workspace.ostask.idle->slot = shared.ostask.first;

    asm ( "mov sp, %[reset_sp]"
      "\n  cpsid aif, #0x10"
      :
      : [reset_sp] "r" ((&workspace.svc_stack)+1) );

    idle_task();
  }

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) idle_task()
{
  // Yield will usually check for other OSTasks that want to be run
  // and schedule one or return immediately. When called by the core's
  // idle OSTask, it will not return until an OSTask has become runnable
  // (and that one yields or blocks).
  asm ( "svc %[yield]"
    "\n  b idle_task"
    :
    : [yield] "i" (OSTask_Yield) );
}

void __attribute__(( naked )) reset_handler()
{
  PANIC;
}

void __attribute__(( naked )) undefined_instruction_handler()
{
  for (;;) asm ( "wfi" );
}

void save_task_state( svc_registers *regs )
{
  workspace.ostask.running->regs = *regs;
  if (workspace.ostask.running->regs.lr == 0) PANIC;
}

void NORET execute_swi( svc_registers *regs, int number )
{
  PANIC;
}

void unexpected_task_return()
{
  PANIC;
}

static inline OSTask **irq_task_ptr( uint32_t number )
{
  uint32_t sources = shared.ostask.number_of_interrupt_sources;
  OSTask **core_interrupts = &shared.ostask.irq_tasks[sources * workspace.core];
  return &core_interrupts[number];
}

static inline void *put_to_sleep( OSTask **head, void *p )
{
  OSTask *tired = p;
  uint32_t time = tired->regs.r[0];

  OSTask *t = *head;
  if (t == 0) {
    *head = tired;
  }
  else {
    if (t->regs.r[0] > time) {
      t->regs.r[0] -= time;
      dll_attach_OSTask( tired, head );
    }
    else {
      while (t->next != *head && t->regs.r[0] < time) {
        time -= t->regs.r[0];
        t = t->next;
      }
      tired->regs.r[0] = time;
      OSTask *tail = t->next;
      dll_attach_OSTask( tired, &tail );
    }
  }

  return 0;
}

static inline void *wakey_wakey( OSTask **headptr, void *p )
{
  p = p;

  OSTask *head = *headptr;
  OSTask *t = head;

  if (t == 0) return 0;
  if (0 < --t->regs.r[0]) return 0;

  OSTask *end = t;

  while (t->regs.r[0] == 0 && t != head) {
    end = t;
    t = t->next;
  }

  dll_detach_OSTasks_until( headptr, end );

  return head;
}

static inline void *add_woken( OSTask **headptr, void *p )
{
  dll_insert_OSTask_list_at_head( p, headptr );

  return 0;
}

static void sleeping_tasks_add( OSTask *tired )
{
  mpsafe_manipulate_OSTask_list( &shared.ostask.sleeping, put_to_sleep, tired );
}

static void sleeping_tasks_tick()
{
  OSTask *list = mpsafe_manipulate_OSTask_list( &shared.ostask.sleeping, wakey_wakey, 0 );
  if (list != 0)
    mpsafe_manipulate_OSTask_list( &shared.ostask.runnable, add_woken, list );
}

void NORET ostask_svc( svc_registers *regs, int number )
{
  OSTask *running = workspace.ostask.running;
  OSTask *resume = 0;

  // Read this now, another core might run the task to completion
  // before we get to the end of this routine.
  OSTaskSlot *currently_mapped_slot = running->slot;

  if (0 == (regs->spsr & 15)) {
    asm ( "mrs %[sp], sp_usr"
      "\n  mrs %[lr], lr_usr"
        : [sp] "=r" (running->banked_sp_usr)
        , [lr] "=r" (running->banked_lr_usr) );
  }

  switch (number) {
  case OSTask_Yield:
  case OSTask_Sleep:
    {
      resume = running->next;

      if (running == workspace.ostask.idle) {
        if (resume == running) {
          resume = mpsafe_detach_OSTask_at_head( &shared.ostask.runnable );

          if (resume != 0) {
            save_task_state( regs );

            dll_attach_OSTask( resume, &workspace.ostask.running );
          }
          else {
            // Pause, then drop back to idle task with interrupts enabled,
            // after which the runnable list will be checked again.
            //wait_for_event();
          }
        }
      }
      else {
        save_task_state( regs );

        workspace.ostask.running = resume;

        // It can't be the only task in the list, there's always idle
        if (running->next == running || running->prev == running) PANIC;

        dll_detach_OSTask( running );

        // Removed properly from list
        if (running->next != running || running->prev != running) PANIC;
        if (workspace.ostask.running == running) PANIC;

        // Removing the head leaves the old next at the head
        if (workspace.ostask.running != resume) PANIC;

        if (number == OSTask_Yield) {
          mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, running );

          signal_event(); // Other cores should check runnable list, if waiting
        }
        else {
          sleeping_tasks_add( running );
        }
      }
    }
    break;
  case OSTask_Create:
    {
      OSTask *task = mpsafe_detach_OSTask_at_head( &shared.ostask.task_pool );

      if (task->next != task || task->prev != task) PANIC;

      task->slot = running->slot;
      task->regs.lr = regs->r[0];
      task->regs.spsr = 0x10;
      task->banked_sp_usr = regs->r[1];
      task->banked_lr_usr = (uint32_t) unexpected_task_return;
      task->regs.spsr = 0x10;
      task->regs.r[0] = ostask_handle( task );
      task->regs.r[1] = regs->r[2];
      task->regs.r[2] = regs->r[3];
      task->regs.r[3] = regs->r[4];
      task->regs.r[4] = regs->r[5];

      // Keep the new task on the current core, in case it's a device
      // driver than needs it. Any Sleep or Yield indicates that the
      // task doesn't care what core it runs on.
      // TODO: decide whether blocking on a lock should maintain core
      // or not.
      OSTask *next = running->next;
      dll_attach_OSTask( task, &next );
    }
    break;
  case OSTask_RegisterInterruptSources:
    {
      if (shared.ostask.number_of_interrupt_sources != 0) PANIC;
      shared.ostask.number_of_interrupt_sources = regs->r[0];
      uint32_t array_size = shared.ostask.number_of_interrupt_sources
                          * shared.ostask.number_of_cores 
                          * sizeof( OSTask * );
      void *memory = system_heap_allocate( array_size );

      if ((uint32_t) memory == 0xffffffff) PANIC;

      shared.ostask.irq_tasks = memory;

      memset( shared.ostask.irq_tasks, 0, array_size );
    }
    break;
  case OSTask_EnablingInterrupt:
    {
      regs->spsr |= 0x80;
    }
    break;
  case OSTask_WaitForInterrupt:
    {
      if (0 == (regs->spsr & 0x80)) PANIC; // Must always have interrupts disabled

      *irq_task_ptr( regs->r[0] ) = running;

      save_task_state( regs );

      resume = running->next;

      workspace.ostask.running = resume;

      // It can't be the only task in the list, there's always idle
      if (running->next == running || running->prev == running) PANIC;

      dll_detach_OSTask( running );

      // Removed properly from list
      if (running->next != running || running->prev != running) PANIC;
      if (workspace.ostask.running == running) PANIC;

      // Removing the head leaves the old next at the head
      if (workspace.ostask.running != resume) PANIC;
    }
    break;
  case OSTask_InterruptIsOff:
    {
      regs->spsr &= ~0x80;
      // move to tail of workspace.running
      PANIC;
    }
    break;
  case OSTask_Tick:
    {
      sleeping_tasks_tick();
    }
    break;
  default:
    PANIC;
  }

  if (resume != 0) {
    if (0 == (resume->regs.spsr & 15)) {
      asm ( "msr sp_usr, %[sp]"
        "\n  msr lr_usr, %[lr]"
          :
          : [sp] "r" (resume->banked_sp_usr)
          , [lr] "r" (resume->banked_lr_usr) );
    }

    if (resume->slot != currently_mapped_slot) {
      PANIC; // Untested
      mmu_switch_map( resume->slot->mmu_map );
    }
  }

  // Restore the stack pointer to where it was before the SVC
  // (nothing is going to use it; interrupts are disabled.)
  // (Make no function calls after this point!)

  asm (
      "add sp, %[regs], %[size]"
      :
      : [regs] "r" (regs)
      , [size] "i" (sizeof( svc_registers )) );

  if (resume != 0) {
    regs = &resume->regs;
  }

  // Resume after the SWI
  register svc_registers *lr asm ( "lr" ) = regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

void NORET execute_svc( svc_registers *regs )
{
  OSTask *caller = workspace.ostask.running;

  uint32_t number = get_svc_number( regs->lr );
  uint32_t swi = (number & ~Xbit); // A bit of RISC OS creeping in!

  switch (swi) {
  case OSTask_Yield ... OSTask_Yield + 63:
    ostask_svc( regs, number );
  default:
    execute_swi( regs, number );
  }
}

void __attribute__(( naked )) svc_handler()
{
  asm volatile (
      // This should detect if the C portion of this function gets
      // complex enough that the code generator needs stack.
      "\n.ifne .-svc_handler"
      "\n  .error \"Kernel_default_svc compiled code includes instructions before srsdb\""
      "\n.endif"

      "\n  srsdb sp!, #0x13 // Store return address and SPSR (SVC mode)"
      "\n  push { r0-r12 }  // and all the non-banked registers"
  );

  svc_registers *regs;
  asm volatile ( "mov %[regs], sp" : [regs] "=r" (regs) );

  execute_svc( regs );

  __builtin_unreachable();
}

#define PROVE_OFFSET( o, t, n ) \
  while (((uint32_t) &((t *) 0)->o) != (n)) \
    asm ( "  .error \"Code assumes offset of " #o " in " #t " is " #n "\"" );

void __attribute__(( naked, noreturn )) irq_handler()
{
  // One interrupt only...

  asm volatile (
        "sub lr, lr, #4"
    "\n  srsdb sp!, #0x12 // Store return address and SPSR (IRQ mode)" );

  OSTask *interrupted_task;

  {
    // Need to be careful with this, that the compiler doesn't insert any code to
    // set up lr using another register, corrupting it.
    PROVE_OFFSET( regs, OSTask, 0 );
    PROVE_OFFSET( lr, svc_registers, 13 * 4 );
    PROVE_OFFSET( spsr, svc_registers, 14 * 4 );
    PROVE_OFFSET( banked_sp_usr, OSTask, 15 * 4 );
    PROVE_OFFSET( banked_lr_usr, OSTask, 16 * 4 );

    register OSTask **running asm ( "lr" ) = &workspace.ostask.running; 
    asm volatile (
        "\n  ldr lr, [lr]       // lr -> running"
        "\n  stm lr!, {r0-r12}  // lr -> lr/spsr"
        "\n  pop { r0, r1 }     // Resume address, SPSR"
        "\n  stm lr!, { r0, r1 }// lr -> banked_sp_usr"
        "\n  tst r1, #0xf"
        "\n  stmeq lr, { sp, lr }^ // Does not update lr, so..."
        "\n  sub %[task], lr, #15*4 // restores its value, in either case"
        : [task] "=r" (interrupted_task)
        , "=r" (running)
        : "r" (running)
        ); // Calling task state saved, except for SVC sp & lr
    asm volatile ( "" : : "r" (running) );
    // lr really, really used (Oh! Great Optimiser, please don't assume
    // it's still pointing to workspace.ostask.running, I beg you!)
  }

  svc_registers *regs = &interrupted_task->regs;
  uint32_t interrupted_mode = regs->spsr & 0x1f;

  if (interrupted_mode != 0x10) PANIC;
  // Legacy RISC OS code allows for SWIs to be interrupted, this is
  // a Bad Thing, and I'll not think about it just yet.

  uint32_t interrupt_number = 0;

  OSTask *irq_task = *irq_task_ptr( interrupt_number );

  // The interrupted task stays on this core, this shouldn't take long!

  if (irq_task == 0) PANIC;

  // New head of core's running list
  dll_attach_OSTask( irq_task, &workspace.ostask.running );

  // The interrupt task is always in OSTask_WaitForInterrupt

  if (irq_task->slot != interrupted_task->slot) {
    // IRQ stack is core-specific, so switching slots does not affect
    // its mapping.
    // (We can call this routine without the stack disappearing.)
    PANIC;
    mmu_switch_map( irq_task->slot->mmu_map );
  }

  asm (
    "\n  msr sp_usr, %[usrsp]"
    "\n  msr lr_usr, %[usrlr]"
    :
    : [usrsp] "r" (irq_task->banked_sp_usr)
    , [usrlr] "r" (irq_task->banked_lr_usr)
  );

  register svc_registers *lr asm ( "lr" ) = &irq_task->regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

static void setup_processor_vectors()
{
  struct vectors {
    uint32_t reset; // ldr pc, ..._vec
    uint32_t undef;
    uint32_t svc;
    uint32_t prefetch;
    uint32_t data;
    uint32_t unused_vector;
    uint32_t irq;
    uint32_t fiq[1024 - 14];

    void *reset_vec;
    void *undef_vec;
    void *svc_vec;
    void *prefetch_vec;
    void *data_vec;
    void *unused; // Needed to keep the distance the same
    void *irq_vec;
  } *vectors = (void*) 0xffff0000;

  memory_mapping vector_page = {
    .base_page = claim_contiguous_memory( 0x1 ),
    .pages = 0x1,
    .va = 0xffff0000,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };
  if (vector_page.base_page == 0xffffffff) PANIC;
  map_memory( &vector_page );

  int32_t vector_offset = offset_of( struct vectors, reset_vec ) - 8;

  vectors->reset         = 0xe59ff000 + vector_offset; // ldr pc, ..._vec
  vectors->undef         = 0xe59ff000 + vector_offset;
  vectors->svc           = 0xe59ff000 + vector_offset;
  vectors->prefetch      = 0xe59ff000 + vector_offset;
  vectors->data          = 0xe59ff000 + vector_offset;
  vectors->unused_vector = 0xeafffffe; // for (;;) {}
  vectors->irq           = 0xe59ff000 + vector_offset;
  vectors->fiq[0]        = 0xeafffffe; // for (;;) {}

  vectors->reset_vec     = reset_handler;
  vectors->undef_vec     = undefined_instruction_handler;
  vectors->svc_vec       = svc_handler;
  vectors->prefetch_vec  = prefetch_handler;
  vectors->data_vec      = data_abort_handler;
  vectors->unused        = 0; // Needed to keep the distance the same
  vectors->irq_vec       = irq_handler;

  // Give all those modes a stack to work with
  asm ( "msr sp_und, %[stack]" : : [stack] "r" ((&(workspace.ostask.und_stack)) + 1) );
  asm ( "msr sp_abt, %[stack]" : : [stack] "r" ((&(workspace.ostask.abt_stack)) + 1) );
  asm ( "msr sp_irq, %[stack]" : : [stack] "r" ((&(workspace.ostask.irq_stack)) + 1) );
  asm ( "msr sp_fiq, %[stack]" : : [stack] "r" ((&(workspace.ostask.fiq_stack)) + 1) );
}
