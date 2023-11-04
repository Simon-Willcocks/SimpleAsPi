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

typedef struct __attribute__(( packed, aligned( 256 ) )) {
  uint32_t       control;
  uint32_t       res1;
  uint32_t       timer_prescaler;
  uint32_t       GPU_interrupts_routing;
  uint32_t       Performance_Monitor_Interrupts_routing_set;
  uint32_t       Performance_Monitor_Interrupts_routing_clear;
  uint32_t       res2;
  uint32_t       Core_timer_access_LS_32_bits; // Access first when reading/writing 64 bits.
  uint32_t       Core_timer_access_MS_32_bits;
  uint32_t       Local_Interrupt_routing0;
  uint32_t       Local_Interrupts_routing1;
  uint32_t       Axi_outstanding_counters;
  uint32_t       Axi_outstanding_IRQ;
  uint32_t       Local_timer_control_and_status;
  uint32_t       Local_timer_write_flags;
  uint32_t       res3;
  uint32_t       Core_timers_Interrupt_control[4];
  uint32_t       Core_Mailboxes_Interrupt_control[4];
  uint32_t       Core_IRQ_Source[4];
  uint32_t       Core_FIQ_Source[4];
  struct {
    uint32_t       Mailbox[4]; // Write only!
  } Core_write_set[4];
  struct {
    uint32_t       Mailbox[4]; // Read/write
  } Core_write_clear[4];
} QA7;

