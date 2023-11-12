/* Copyright 2021-2023 Simon Willcocks
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

#ifndef CK_TYPES_H
#define CK_TYPES_H

typedef unsigned long long uint64_t;
typedef unsigned        uint32_t;
typedef int             int32_t;
typedef unsigned short  uint16_t;
typedef short           int16_t;
typedef unsigned char   uint8_t;
typedef signed char     int8_t;
typedef unsigned        size_t;
typedef unsigned        bool;
#define true  (0 == 0)
#define false (0 != 0)

#define number_of( arr ) (sizeof( arr ) / sizeof( arr[0] ))
#define offset_of( T, E ) ((uint32_t) &((T*)0)->E)

static const uint32_t NF = (1 << 31);
static const uint32_t ZF = (1 << 30);
static const uint32_t CF = (1 << 29);
static const uint32_t VF = (1 << 28);

static const uint32_t Xbit = (1 << 17);

// Copy of the registers stored for an SVC instruction; doesn't include
// the user stack pointer or link registers.
typedef struct __attribute__(( packed )) svc_registers {
  uint32_t r[13];
  uint32_t lr;
  uint32_t spsr;
} svc_registers;

// This is rather RISC OS specific, but the task management
// SWIs return errors in this format.
typedef const struct {
  uint32_t code;
  char desc[];
} error_block;

#endif
