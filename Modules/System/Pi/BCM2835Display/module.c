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

// QA7 module, for use on Raspberry Pi 2 - 3

#include "CK_types.h"
#include "ostaskops.h"

typedef struct workspace workspace;

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "BCM2835Display";
const char help[] = "BCM2835Display\t0.01 (" CREATION_DATE ")";

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

struct workspace {
  uint32_t pa;
  uint32_t stack[63];
};

void open_display( uint32_t handle, workspace *ws )
{
  uint32_t space_on_stack[30];

#ifndef DEBUG__BREAKME1
  uint32_t mr = (uint32_t) space_on_stack;
  mr = (mr + 15) & ~15;
  uint32_t *mailbox_request = (void*) mr;
#else
  // arm-none-eabi-gcc-9.2.1 seems to generate nonsense for this...
  uint32_t *mailbox_request = (uint32_t) space_on_stack;
  while (0 != 15 & ((uint32_t) mailbox_request)) mailbox_request++;
#endif

  // Order is important, at least for QEMU, I think

  int i = 0;
  mailbox_request[i++] = 26 * 4; // Message buffer size
  mailbox_request[i++] = 0; // request
  mailbox_request[i++] = 0x00048004;  // Set virtual (buffer) width/height
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 1920;
  mailbox_request[i++] = 1092; // 12 invisible pixels to make the buffer 8MiB
  mailbox_request[i++] = 0x00048003; // Set physical (display) width/height
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 1920;
  mailbox_request[i++] = 1080;
  mailbox_request[i++] = 0x00048005; // Set depth
  mailbox_request[i++] = 4;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 32;
  mailbox_request[i++] = 0x00048006; // Pixel order
  mailbox_request[i++] = 4;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 0; // 0: BGR, 1: RGB
  mailbox_request[i++] = 0x00040001, // Allocate buffer
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  uint32_t *frame_buffer = mailbox_request + i;
  mailbox_request[i++] = 2 << 20;
  uint32_t *frame_buffer_size = mailbox_request + i;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 0; // No more tags

  uint32_t pa = Task_PhysicalFromVirtual( mailbox_request, *mailbox_request );

  register uint32_t req asm( "r0" ) = pa;
  asm volatile (
      "svc #0x1088" // Channel 8
      :
      : "r" (req)
      : "lr", "cc" );

  ws->pa = *frame_buffer;
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  // This in SVC mode, can't do things like wait for responses from
  // external hardware!
  workspace *ws = rma_claim( sizeof( workspace ) );
  *private = ws;

  register void *start asm( "r0" ) = open_display;
  register void *sp asm( "r1" ) = ws + 1;
  register workspace *r1 asm( "r2" ) = ws;

  register uint32_t handle asm( "r0" );

  asm volatile ( "svc %[swi]" // volatile in case we ignore output
    : "=r" (handle)
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void __attribute__(( naked )) init()
{
  register struct workspace **private asm ( "r12" );
  register char const *env asm ( "r10" );
  register uint32_t instantiation asm ( "r11" );

  // Move r12 into argument register
  asm volatile ( "push { lr }" );

  c_init( private, env, instantiation );

  asm ( "pop { pc }" );
}

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}

