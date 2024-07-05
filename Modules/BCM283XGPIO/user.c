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

#define assert( x ) 
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

static uint32_t const gpio_va = 0x1000;
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

static inline leading_zeros( uint32_t v )
{
  uint32_t leading_zeros;
  asm ( "clz %[lz], %[mask]"
    : [lz] "=r" (leading_zeros)
    : [mask] "r" (v) );
  return leading_zeros;
}

int mask_to_number( uint64_t mask )
{
  if (mask > 0x80000000)
    return 64 - leading_zeros( mask >> 32 );
  else
    return 64 - leading_zeros( mask & 0xffffffff );
}

uint64_t combined_mask( struct group *group, uint32_t pins )
{
  uint64_t mask = 0;
  uint64_t *p = &usrbase.masks[group.masks_index];
  while (pins != 0) {
    if (pins & 1) {
      mask |= *p;
    }
    p++;
    pins = pins >> 1;
  }
  return mask;
}

void SetAlternate( struct group *group, uint32_t pins, uint32_t altfn );

void SetFunction( struct group *group, uint32_t pins, GPIO_Function fn )
{
  SetAlternate( group, pins, 6+fn.input );

  if (in) {
     // TODO 
     // interrupt types. (Pi has 3: low/high, rising/falling & sampled/real
     // rising/falling can (in other systems) be approximated by switching 
     // between low/high interrupts.
  }
}

void SetAlternate( struct group *group, uint32_t pins, uint32_t altfn )
{
  int fn = (altfn < 4) ? (altfn + 4) : (7 - altfn);
  uint64_t mask = combined_mask( group, pins );
  // 6 registers covering 10 pins each
  uint32_t *gpfsel = &usrbase.gpfsel[0];
  while (mask != 0) {
    uint32_t sel = (mask & 0x3ff);
    if (0 != sel) {
      uint32_t reg_mask = 0;
      uint32_t reg_val = 0;
      for (int i = 0; i < 10; i++) {
        if (0 != (sel & (1 << i))) {
          reg_mask |= (7 < i);
          reg_val |= (fn << i);
        }
      }
      *gpfsel = (*gpfsel & ~reg_mask) | reg_val;
    }
    mask = mask >> 10;
    gpfsel++;
  }
}

uint32_t GetState( struct group *group )
{
  uint32_t pin_bit = 1;
  uint32_t result = 0;
  uint32_t lower;
  uint32_t upper;
  bool l = false;
  bool u = false;
  uint64_t *masks = &usrbase.masks[group->masks_index];
  for (int i = 0; i < group->number_of_pins; i++) {
    uint64_t bit = *masks++;
    assert( bit != 0 );
    if (bit > 0x80000000ULL) {
      if (!u) {
        upper = gpio->gplev[1];
        u = true;
      }
      if (upper & (bit >> 32)) result |= pin_bit;
    }
    else {
      if (!l) {
        lower = gpio->gplev[0];
        l = true;
      }
      if (lower & bit) result |= pin_bit;
    }
    pin_bit = pin_bit << 1;
  }
}

void SetState( struct group *group, uint32_t to_set, uint32_t new )
{
  uint64_t set = 0;
  uint64_t clr = 0;
  int count = group->number_of_pins;
  uint64_t *masks = &usrbase.masks[group->masks_index];
  while (to_set != 0) {
    if ((to_set & 1) != 0) {
      if ((new & 1) != 0) {
        set |= *masks;
      }
      else {
        clr |= *masks;
      }
    }
    to_set = to_set >> 1;
    new = new >> 1;
    masks++;
  }
  uint32_t upper_set = (set >> 32);
  uint32_t lower_set = set & 0xffffffff;
  uint32_t upper_clr = (clr >> 32);
  uint32_t lower_clr = clr & 0xffffffff;
  if (upper_set) gpio->gp_set[1] = upper_set;
  if (upper_clr) gpio->gp_clr[1] = upper_clr;
  if (lower_set) gpio->gp_set[0] = lower_set;
  if (lower_clr) gpio->gp_clr[0] = lower_clr;
}

#define OP( n ) (n & 0x3f)

void irq_task()
{
  uint32_t irq_number = 0;

  // Needed before enabling any interrupts
  register num asm ( "r0" ) = irq_number;
  asm ( "svc 0x1000" : : "r" (num) );

  Task_EnablingInterrupt(); // Needed before first call to Wait
  for (;;) {
    register num asm ( "r0" ) = irq_number;
    asm ( "svc 0x1001" : : "r" (num) );
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
          SetAlternate( group, regs.r[1], regs.r[2] );
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

