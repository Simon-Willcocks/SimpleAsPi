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

typedef struct __attribute__(( packed, aligned( 256 ) )) {
  uint32_t data;                                // 0x00
  uint32_t receive_status_error_clear;          // 0x04
  uint32_t res0[4];
  uint32_t flags;                               // 0x18
  uint32_t res1[2];
  uint32_t integer_baud_rate_divisor;           // 0x24
  uint32_t fractional_baud_rate_divisor;        // 0x28
  uint32_t line_control;                        // 0x2c
  uint32_t control;                             // 0x30
  uint32_t interrupt_fifo_level_select;         // 0x34
  uint32_t interrupt_mask;                      // 0x38
  uint32_t raw_interrupt_status;                // 0x3c
  uint32_t masked_interrupt_status;             // 0x40
  uint32_t interrupt_clear;                     // 0x44
  uint32_t dma_control;                         // 0x48
  uint32_t res2[(0x80-0x4c)/4];
  uint32_t test_control;                        // 0x80
  uint32_t integration_test_input;              // 0x84
  uint32_t integration_test_output;             // 0x88
  uint32_t test_data;                           // 0x8c
} UART;

// Flags @ 0x18
enum { UART_Busy = 1 << 3,
       UART_RxEmpty = 1 << 4,
       UART_TxFull = 1 << 5,
       UART_RxFull = 1 << 6,
       UART_TxEmpty = 1 << 7
     };

