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

#include "common.h"
#include "gpio.h"

void manage_gpio( uint32_t handle, workspace *ws );

void __attribute__(( naked, noreturn )) gpio_task( uint32_t handle,
                                                    workspace *ws )
{
  // Running in usr32 mode, no stack, but my very own slot
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0", "r1" );

  manage_gpio( handle, ws );

  __builtin_unreachable();
}

static uint32_t const gpio_va = 0x7000;
GPIO volatile *const gpio = (void*) gpio_va;
extern struct {
  struct group {
    uint8_t number_of_pins;
    uint8_t masks_index;
  } groups[32];
  uint32_t tasks[32]; // One per group
  uint64_t masks[54];
  uint64_t allocated;
  uint8_t first_unused_mask;
} usrbase;

static uint32_t const magic = 0x4f495047;

struct group *group_from_handle( uint32_t handle )
{
  uint32_t n = handle ^ magic;
  if (n > number_of( usrbase.groups )) return 0;
  if (usrbase.groups[n].number_of_pins == 0) return 0;
  return &usrbase.groups[n];
}

uint32_t group_handle( struct group *g )
{
  return (g - usrbase.groups) ^ magic;
}

void ReleaseGroup( struct group *group )
{
  
}

void SetFunction( struct group *group, uint32_t pins, regs.r[2] )
{
  
}

void SetAlternate( struct group *group, uint32_t altfn )
{
  
}

uint32_t GetState( struct group *group )
{
  
}

void SetState( struct group *group, uint32_t to_set, uint32_t new )
{
  
}

#define OP( n ) (n & 0x3f)

void irq_task()
{
  Task_EnablingInterrupt(); // Needed before first call to Wait
  for (;;) {
    GPIO_WaitForInterrupt( n );
  }
}

void create_irq_task( workspace *ws )
{
  register void *start asm( "r0" ) = adr( irq_task );
  register void *sp asm( "r1" ) = 0x8c00;
  register workspace *r1 asm( "r2" ) = ws;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void manage_gpio( uint32_t handle, workspace *ws )
{
  uint32_t gpio_page = 0x3f00b000 >> 12;
  Task_MapDevicePages( gpio_va, gpio_page, 1 );
  svc_registers regs;

  memset( &usrbase, 0, sizeof( usrbase ) );

  create_irq_task();

  for (;;) {
    queued_task client = Task_QueueWait( ws->queue );
    Task_GetRegisters( client.task_handle, &regs );

    switch (client.swi) {
    case OP( gpio_SystemName ):
      regs.r[0] = (uint32_t) "BCM283XGPIO";
      break;
    case OP( gpio_ClaimPinGroup ):
      {
      struct group *group = usrbase.groups;
      while (group->number_of_pins != 0) group++;
      group->masks_index = usrbase.first_unused_mask;
      svc_registers tmp = {};
      tmp.lr = (uint32_t) get_pins;
      tmp.spsr = regs.spsr;
      tmp.r[12] = regs->r[0];
      int i = 0;
      do {
        if (i == 10) i = 0;

        if (i == 0) {
          // Get up to ten more pins.
          Task_RunThisForMe( tmp.task_handle, &tmp );
        }

        if (tmp.r[i] >= 0 && tmp.r[i] < 54) {
          uint64_t mask = 1ULL << tmp.r[i];
          if (0 != (usrbase.allocated & mask)) {
            regs.spsr |= VF;
            static error_block error = { 0, "Pin already allocated" };
            regs.r[0] = (uint32_t) &error;
          }
          usrbase.masks[usrbase.first_unused_mask++] = mask;
          i++;
        }
        else if (tmp.r[i] != -1U) {
          regs.spsr |= VF;
          static error_block error = { 0, "Pin out of range 0-53" };
          regs.r[0] = (uint32_t) &error;
        }
      } while (tmp.r[i] != -1U);

      if (0 == (regs.spsr & VF)) {
        group->number_of_pins = usrbase.first_unused_mask
                                        - group->masks_index;
        regs.r[0] = group_handle( group );
      }
      else {
        // Release any allocated pins, clearing the mask and removing them
        // from the allocated bitmap.
        asm ( "bkpt 1" );
      }
      }
      break;
    default:
      {
        struct group *group = group_from_handle( regs.r[0] );
        if (group == 0) {
          static error_block error = { 0, "Not an allocated group" };
          regs.r[0] = (uint32_t) &error;
          regs.spsr |= VF;
          break;
        }

        switch (client.swi) {
        case OP( gpio_ReleaseGroup ):
          ReleaseGroup( group );
          break;
        case OP( gpio_SetFunction ):
          SetFunction( group, regs.r[1], regs.r[2] );
          break;
        case OP( gpio_SetAlternate ):
          SetAlternate( group, regs.r[1] );
          break;
        case OP( gpio_GetState ):
          GetState( group );
          break;
        case OP( gpio_SetState ):
          SetState( group, regs.r[1], regs.r[2] );
          break;
        case OP( gpio_WaitForInterrupt ):
          // The task is already detatched from the runnable tasks, the 
          // manage_gpio task is its controller.
          // I need to change its controller to the IRQ task, then make it
          // available to that task to resume.
          Task_ChangeController( client.task_handle, irq_task );
          // The irq task still doesn't know it has control over the client
          usrbase.tasks[group_number( group )] = client.task_handle;
          // Now it does, enable the interrupts
          PANIC;
          break;
        default:
          static error_block error = { 0, "Unsupported GPIO SWI" };
          regs.r[0] = (uint32_t) &error;
          regs.spsr |= VF;
          break;
        }
      }
    }

    Task_ReleaseTask( client.task_handle, &regs );
  }
}

