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

typedef struct OSTaskSlot OSTaskSlot;
typedef struct OSTask OSTask;

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
}

void NORET execute_swi( svc_registers *regs, int number )
{
  PANIC;
}

void NORET ostask_svc( svc_registers *regs, int number )
{
  OSTask *resume = 0;

  switch (number) {
  case OSTask_Yield:
  case OSTask_Sleep:
    {
      OSTask *running = workspace.ostask.running;
      OSTask *resume = running->next;

      save_task_state( regs );

      if (running == resume) {
        // Only idle on this core, anything else runnable?
        resume = mpsafe_detach_OSTask_at_head( &shared.ostask.runnable );
        if (resume == 0) {
          // Symptom: LED occasionally blinks, more often solid green
          // Thoughts: There are no interrupts to exit wfi
          //           There are also no regular events (afaik)
          // Solution: Restore this instruction when interrupts are
          //           enabled. Perhaps wfe instead? (And sev when
          //           inserting OSTasks into an empty running queue?)
          //asm ( "wfi" );
          break; // Drop back to idle task, enabling interrupts
        }
        else {
          dll_attach_OSTask( resume, &workspace.ostask.running );
          if (resume != running->next) PANIC;
        }
      }

      // resume = running->next, resume != running

      workspace.ostask.running = resume;

      if (resume->slot != running->slot) {
        PANIC; // Untested
        mmu_switch_map( resume->slot->mmu_map );
      }

      if (running != workspace.ostask.idle) {
        dll_detach_OSTask( running );
        mpsafe_insert_OSTask_at_tail( &shared.ostask.runnable, running );
      }
    }
    break;
  }

  // Restore the stack pointer to where it was before the SVC
  // (nothing is going to use it; interrupts are disabled.)
  // (Make no function calls after this point!)

  asm (
      "add sp, %[regs], %[size]"
      :
      : [regs] "r" (regs)
      , [size] "i" (sizeof( svc_registers )) );

  if (resume != 0) regs = &resume->regs;

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

void __attribute__(( naked )) irq_handler()
{
  PANIC;
  
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
