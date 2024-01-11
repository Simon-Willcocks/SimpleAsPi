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
  Task_LogString( "Opening BCM2835 display\n", 0 );

  uint32_t space_on_stack[30];

  uint32_t mr = (uint32_t) space_on_stack;
  mr = (mr + 15) & ~15;
  uint32_t *mailbox_request = (void*) mr;

  // Order is important, at least for QEMU, I think

  int i = 0;
  mailbox_request[i++] = 26 * 4; // Message buffer size
  mailbox_request[i++] = 0; // request
  mailbox_request[i++] = 0x00048004;  // Set virtual (buffer) width/height
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 1920;
#ifdef QEMU
  mailbox_request[i++] = 1092; // 12 invisible pixels to make the buffer almost 8MiB
#else
  mailbox_request[i++] = 1080; // Real hardware doesn't like the above trick
#endif
  mailbox_request[i++] = 0x00048003; // Set physical (display) width/height
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 1920;
  mailbox_request[i++] = 1080;
  mailbox_request[i++] = 0x00048005; // Set depth
  mailbox_request[i++] = 4;
  mailbox_request[i++] = 0;
  uint32_t *bits_per_pixel = mailbox_request + i;
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

  // A better approach would be to create a pipe over the data and pass
  // that to the server; it can then get a physical address from the
  // local end of the pipe. Being allowed to specify random physical
  // addresses is a bad idea.
  uint32_t pa = Task_PhysicalFromVirtual( mailbox_request, *mailbox_request );

  Task_LogString( "BCM2835 display opening\n", 0 );

  register uint32_t req asm( "r0" ) = pa;
  asm volatile (
      "svc #0x1088" // Channel 8
      :
      : "r" (req)
      : "lr", "cc", "memory" );

  // Make sure we can see what the GPU wrote
  // This can be shown to be essential by commenting it out and
  // checking if the value of mailbox_request[1] is non-zero
  Task_MemoryChanged( mailbox_request, *mailbox_request );

if (mailbox_request[1] == 0x80000000 // OK
&& mailbox_request[4] == 0x80000008
&& mailbox_request[9] == 0x80000008
&& mailbox_request[14] == 0x80000004
&& mailbox_request[18] == 0x80000004
&& mailbox_request[22] == 0x80000008 // OK

&& (0xfffff & mailbox_request[23]) == 0 // OK

// && mailbox_request[24] == 0x7ff800  // Not OK (on real hardware)
//&& mailbox_request[24] == 1920 * 1080 * 4 // No
&& mailbox_request[24] >= 1920 * 1080 * 4 // Yes 
&& mailbox_request[24] < 0x800000 // Yes

// && mailbox_request[24] >= 1920 * 1092 * 4 // No!
// && mailbox_request[24] <= 0x800000
&& *bits_per_pixel == 32 // OK

&& *frame_buffer_size == mailbox_request[24] // OK

// && *frame_buffer_size <= (1920 * 1096 * 4) // No
&& *frame_buffer_size <= (1920 * 1200 * 4) // OK

&& *frame_buffer == mailbox_request[23] // OK

&& *frame_buffer >= 0x80000000 // Yes (real hardware)
&& *frame_buffer >= 0xc0000000 // Yes
// && *frame_buffer >= 0xe0000000 // No
&& *frame_buffer < 0xd0000000 // Yes
) {
  register uint32_t pin asm( "r0" ) = 27; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 100;
  register uint32_t off asm( "r2" ) = 200;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );
}

  Task_LogString( "BCM2835 display open, base ", 0 );
  Task_LogHex( *frame_buffer );
  Task_LogString( ", size ", 0 );
  Task_LogHex( *frame_buffer_size );
  Task_LogString( ", mapped at ", 0 );

  uint32_t size = *frame_buffer_size;
  size = (size + 0xfff) >> 12;

  // Correct for something... Returns an address between 0xc0000000
  // and 0xd0000000, but needs to be accessed with lower physical
  // addresses. I can't remember why this is needed!
  uint32_t base = (0x3fffffff & *frame_buffer) >> 12;
  uint32_t *fb = Task_MapFrameBuffer( base, 0x800 ); // size );

  Task_LogHex( (uint32_t) fb );
  Task_LogNewLine();

  memset( fb, 0xcc, *frame_buffer_size );

  Task_MemoryChanged( fb, *frame_buffer_size );

  Task_LogString( "Filled\n", 0 );
  for (;;) { Task_Sleep( 10000 ); }
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
  register uint32_t sp asm( "r1" ) = aligned_stack( ws + 1 );
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

