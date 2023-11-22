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

void manage_gpu( uint32_t handle, workspace *ws );

void __attribute__(( naked, noreturn )) gpu_task( uint32_t handle,
                                                    workspace *ws )
{
  // Running in usr32 mode, no stack, but my very own slot
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0", "r1" );

  manage_gpu( handle, ws );

  __builtin_unreachable();
}

GPU volatile *const gpu = (void*) 0x7000;

void manage_gpu( uint32_t handle, workspace *ws )
{
  uint32_t gpu_page = 0x3f00b000 >> 12;
  Task_MapDevicePages( (uint32_t) gpu, gpu_page, 1 );
  for (;;) {
    Task_Yield();
  }
}

