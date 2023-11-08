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

// The OSTask code doesn't care about the content of the extras, but
// will allocate space for them in OSTask and OSTaskSlot structures.

#include "CK_types.h"

// This include file must define OSTaskSlot_extras and OSTask_extras
#include "ostask_extras.h"

enum {
    OSTask_Yield = 0x300
  , OSTask_Sleep
  , OSTask_Create                       // New OSTask
  , OSTask_Spawn                        // New OSTask in a new slot
  , OSTask_EndTask                      // Last one out ends the slot
  , OSTask_RegisterInterruptSources     // Once only
  , OSTask_EnablingInterrupt            // Disable IRQs while I get
  , OSTask_WaitForInterrupt             // ready to wait.
  , OSTask_InterruptIsOff               // Enable interrupts (I'm done,
                                        // but not yet waiting)
  , OSTask_SwitchToCore                 // Use sparingly!
  , OSTask_Tick                         // For HAL module use only
} OSTaskSWI;

// These routines return the higher level data
OSTaskSlot_extras *OSTaskSlot_extras_now();
OSTask_extras *OSTask_extras_now();

static inline
void Task_EndTask()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_EndTask)
    : "lr", "cc" );
}

static inline
void Task_RegisterInterruptSources( uint32_t n )
{
  register uint32_t number asm ( "r0" ) = n;

  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_RegisterInterruptSources)
    , "r" (number)
    : "lr", "cc" );
}

static inline
void Task_EnablingIntterupt()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_EnablingInterrupt)
    : "lr", "cc" );
}

static inline
void Task_WaitForInterrupt( uint32_t n )
{
  register uint32_t number asm ( "r0" ) = n;

  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_WaitForInterrupt)
    , "r" (number)
    : "lr", "cc" );
}

