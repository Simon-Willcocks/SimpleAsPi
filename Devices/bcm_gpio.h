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

// Note: Alignment is essential for device area, so that GCC doesn't
// generate multiple strb instructions to write a single word.
// Or I could be professional and make every device access an inline
// routine call. Nah!
typedef struct __attribute__(( packed, aligned( 256 ) )) {
  uint32_t gpfsel[6];  // 0x00 - 0x14
  uint32_t res18;
  uint32_t gpset[2];   // 0x1c, 0x20
  uint32_t res24;
  uint32_t gpclr[2];
  uint32_t res30;     // 0x30
  uint32_t gplev[2];
  uint32_t res3c;
  uint32_t gpeds[2];   // 0x40
  uint32_t res48;
  uint32_t gpren[2];
  uint32_t res54;
  uint32_t gpfen[2];
  uint32_t res60;     // 0x60
  uint32_t gphen[2];
  uint32_t res6c;
  uint32_t gplen[2];    // 0x70
  uint32_t res78;
  uint32_t gparen[2];
  uint32_t res84;
  uint32_t gpafen[2];
  uint32_t res90;     // 0x90
  uint32_t gppud;
  uint32_t gppudclk[2];
  uint32_t resa0;
  uint32_t resa4;
  uint32_t resa8;
  uint32_t resac;
  uint32_t test;
} GPIO;

// For gpfsel:
enum { GPIO_Input, GPIO_Output, GPIO_Alt5, GPIO_Alt4,
       GPIO_Alt0, GPIO_Alt1, GPIO_Alt2, GPIO_Alt3 };

static inline void set_state( GPIO *g, int bit, int state )
{
  uint32_t index = 0;
  while (bit >= 10) {
    bit -= 10;
    index++;
  }
  uint32_t shift = bit * 3;
  uint32_t mask = 7 << shift;
  g->gpfsel[index] = (g->gpfsel[index] & ~mask) | (state << shift);
}
