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

#include "CK_types.h"
#include "kernel_swis.h"
#include "ostaskops.h"


void __attribute__(( naked, noreturn )) serve_legacy_swis( uint32_t handle,
                                                           uint32_t queue )
{
  asm volatile (
    "\n.ifne .-serve_legacy_swis"
    "\n  .error \"serve_legacy_swis compiled code needs stack\""
    "\n.endif" );

  // Special use of OS_CallASWIR12 only known to this subsystem.
  // The kernel checks against this task's handle, so it can't
  // be misused.
  // When it returns, the client will have run the SWI and
  // been released on behalf of this controlling task.
  asm volatile (
    "\n  mov r8, r1"
    "\n0:"
    "\n  mov r0, r8"
    "\n  svc %[wait]"
    "\n  svc %[send]"
    "\n  b 0b"
    :
    : [wait] "i" (OSTask_QueueWait | Xbit)
    , [send] "i" (OS_CallASWIR12 | Xbit) );
}
