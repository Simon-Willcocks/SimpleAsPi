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
#include "bcm_gpio.h"
#include "ostaskops.h"

typedef struct workspace workspace;

struct workspace {
  uint32_t task;
  uint32_t queue;
};

void __attribute__(( naked, noreturn )) gpio_task( uint32_t handle,
                                                   workspace *ws );


/*
GPIO Interface

Users may claim groups of pins and manipulate them independent of their
physical numbers; a group may only be claimed by one program at a time.

GPIO_System returns the name of the system. Nothing will be using GPIO 
without knowing what it's running on!

GPIO_GroupPins
  IN: the pins the caller wants to control/use (count, array of numbers)
  OUT: a handle to allow the pins to be manipulated independent of other 
       users
  Note: Up to 32 pins in a group, functionality defaults to input, no 
        interrupts
        The physical pin order is irrelevant after the grouping; so e.g.
        wanting one tx and one rx pin, the user can manipulate them as
        bits 0 and 1, whether they're real pins 1 and 2, 2 and 1, or 76
        and 22.

GPIO_ReleaseGroup
  IN: group handle

GPIO_SetFunction
  IN: group handle
      pins to affect (bit 0 is first pin in group, bit 1 second, etc.)
      function type: Output, Input, with or without interrupts (on rising,
      falling, change, high, low).

GPIO_GetState
  IN: group handle
  OUT: current state of pins

GPIO_SetState
  IN: group handle
      pins to change
      new values
  OUT: current state of pins
  Note: Attempting to change input pins will result in an error
        End result is pins = (pins & ~change) | new
        Implementation may differ e.g.
          outset = new & change, outclr = ~new & change

GPIO_WaitForInterrupt
  IN: group handle
  OUT: current state of pins

Test to run: connect two pairs of pins together, one in, one out,
have two tasks loop, setting the output to 1, waiting for an interrupt
on 2, then clearing the output, etc. Count how often it gets interrupted
in a second.
*/
