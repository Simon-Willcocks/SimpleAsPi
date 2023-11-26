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

// GPIO Interface

// Users may claim groups of pins and manipulate them independent of their
// physical numbers; a group may only be claimed by one program at a time.

enum { gpio_SystemName = 0x400
     , gpio_ClaimPinGroup
     , gpio_ReleaseGroup
     , gpio_SetFunction
     , gpio_SetAlternate
     , gpio_GetState
     , gpio_SetState
     , gpio_WaitForInterrupt };

// Returns the name of the system, or NULL, if there is no available
// GPIO module.
char const *GPIO_SystemName()
{
  register char const *name asm ( "r0" );
  asm ( "svc %[swi]"
    "\n  movvs r0, #0"
    : "=r" (name)
    : [swi] "i" (gpio_SystemName | Xbit)
    : "lr", "cc" );

  return name;
}

// Get a handle on a number of GPIO pins.
// The pins array is a -1 terminated list of up to 32 desired pins.
// The first will be assocated with bit 0 in subsequent calls, the
// second with bit 1, etc.
uint32_t GPIO_ClaimPinGroup( int32_t *pins, error_block **error )
{
  register uint32_t *array asm ( "r0" ) = pins;
  register uint32_t handle asm ( "r0" );
  if (error != 0) {
    asm ( "svc %[swi]"
      "\n  strvs r0, [%[errorp]]"
      "\n  movvs r0, #0"
      : "=r" (handle)
      : [swi] "i" (gpio_ClaimPinGroup | Xbit)
      , "r" (array)
      , [errorp] "r" (error)
      : "lr", "cc" );
  }
  else {
    asm ( "svc %[swi]"
      "\n  movvs r0, #0"
      : "=r" (handle)
      : [swi] "i" (gpio_ClaimPinGroup | Xbit)
      , "r" (array)
      : "lr", "cc" );
  }

  return handle;
}

struct GPIO_Function {
  uint32_t input:1;
  uint32_t interrupt_on_rising_edge:1;
  uint32_t interrupt_on_falling_edge:1;
  uint32_t interrupt_on_high:1;
  uint32_t interrupt_on_low:1;
};

// For normal, software controlled GPIO pins, set as output or input with
// optional interrupt signalling.
void GPIO_SetFunction( uint32_t group, uint32_t pins, GPIO_Function fn )
{
  register uint32_t handle asm ( "r0" ) = group;
  register uint32_t set asm ( "r1" ) = pins;
  register GPIO_Function function asm ( "r2" ) = fn;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (gpio_SetFunction)
    , "r" (handle)
    , "r" (set)
    , "r" (function)
    : "lr", "cc" );
}

// Some systems (notably the Pi) can attach GPIO pins internally to
// controllers such as serial UARTs, I2C bus, etc.
// The user should claim the pins, then call this routine to choose the
// alternate function. Releasing the group will return the pins to the
// default (input, no interrupts) state.
void GPIO_SetAlternate( uint32_t group, uint32_t pins, uint32_t fn )
{
  register uint32_t handle asm ( "r0" ) = group;
  register uint32_t set asm ( "r1" ) = pins;
  register uint32_t function asm ( "r2" ) = fn;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (gpio_SetAlternate)
    , "r" (handle)
    , "r" (set)
    , "r" (function)
    : "lr", "cc" );
}

// Return the state of the pins in the group
uint32_t GPIO_GetState( uint32_t group )
{
  register uint32_t handle asm ( "r0" ) = group;
  register uint32_t pins asm ( "r0" );

  asm ( "svc %[swi]"
    : "=r" (pins)
    : [swi] "i" (gpio_SetFunction)
    , "r" (handle)
    : "lr", "cc" );
  return pins;
}

void GPIO_SetState( uint32_t group, uint32_t change, uint32_t new_state )
{
  register uint32_t handle asm ( "r0" ) = group;
  register uint32_t set asm ( "r1" ) = change;
  register uint32_t value asm ( "r2" ) = new_state;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (gpio_SetFunction)
    , "r" (handle)
    , "r" (set)
    , "r" (value)
    : "lr", "cc" );
}

// When any of the pins in the group signals an interrupt, the state of
// the pins is returned.
// Releasing the group will also cause this function to return.
uint32_t GPIO_WaitForInterrupt( uint32_t group )
{
  register uint32_t handle asm ( "r0" ) = group;
  register uint32_t pins asm ( "r0" );
  asm ( "svc %[swi]"
    : "=r" (pins)
    : [swi] "i" (gpio_WaitForInterrupt)
    , "r" (handle)
    : "lr", "cc" );
  return pins;
}

