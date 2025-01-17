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

// Number of interfaces (HDMI, DisplayPort, ScreenCast?)
// Or is that max number of screens?
// Active screens (with a display associated with it)
// Overlays?

// How about this:
//  The application doesn't need to worry about where its window is
//  being displayed.
//  On return from GetRectangle, the frame buffer/output sprite can
//  be anywhere.

// Could the main frame buffer be a single pixel?

// Map the frame buffer into Slot memory, create pipes for other tasks 
// to write to.
// SpaceFilled pushes to the cache. Always "fill" the whole thing (or
// never do, leave it to the Wimp or some such).


#include "CK_types.h"
#include "ostaskops.h"

typedef struct workspace workspace;

#define MODULE_CHUNK "0x20c0"

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
  uint32_t queue;
  uint32_t stack[61];
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
  mailbox_request[i++] = 1080;
  mailbox_request[i++] = 0x00048003; // Set physical (display) width/height
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 1920;
  mailbox_request[i++] = 1080;
  mailbox_request[i++] = 0x00048005; // Set depth
  mailbox_request[i++] = 4;
  mailbox_request[i++] = 0;
  // uint32_t *bits_per_pixel = mailbox_request + i;
  mailbox_request[i++] = 32;
  mailbox_request[i++] = 0x00048006; // Pixel order
  mailbox_request[i++] = 4;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 1; // 0: BGR, 1: RGB
  mailbox_request[i++] = 0x00040001, // Allocate buffer
  mailbox_request[i++] = 8;
  mailbox_request[i++] = 0;
  uint32_t *frame_buffer = mailbox_request + i;
  mailbox_request[i++] = 2 << 20;
  uint32_t *frame_buffer_size = mailbox_request + i;
  mailbox_request[i++] = 0;
  mailbox_request[i++] = 0; // No more tags

  Task_FlushCache( mailbox_request, *mailbox_request );

  // A better approach would be to create a pipe over the data and pass
  // that to the server; it can then get a physical address from the
  // local end of the pipe. Being allowed to specify random physical
  // addresses is a bad idea.
  uint32_t pa = Task_PhysicalFromVirtual( mailbox_request, *mailbox_request );

  Task_LogString( "BCM2835 display opening\n", 0 );

  register uint32_t req asm( "r0" ) = pa;
  register error_block *error asm( "r0" );
  asm volatile (
      "svc #0x21088" // Channel 8
  "\n  movvc r0, #0"
      : "=r" (error)
      : "r" (req)
      : "lr", "cc", "memory" );

  if (error != 0) {
    Task_LogString( "BCM2835 GPU Mailbox not responding ", 0 );
    return;
  }
  // Make sure we can see what the GPU wrote
  // This can be shown to be essential by commenting it out and
  // checking if the value of mailbox_request[1] is non-zero
  Task_InvalidateCache( mailbox_request, *mailbox_request );

  Task_LogString( "BCM2835 display response ", 0 );
  Task_LogHex( mailbox_request[1] );
  Task_LogNewLine();

  if (0 == mailbox_request[1]) {
    return;
  }

  Task_LogString( "BCM2835 display open, base ", 0 );
  Task_LogHex( *frame_buffer );
  Task_LogString( ", size ", 0 );
  Task_LogHex( *frame_buffer_size );
  Task_LogNewLine();

  // Correct for something... Returns an address between 0xc0000000
  // and 0xd0000000, but needs to be accessed with lower physical
  // addresses. I can't remember why this is needed!
  // I think it has to do with the GPU's caching.

  uint32_t base_page = (0x3fffffff & *frame_buffer) >> 12;
  //uint32_t base_page = *frame_buffer >> 12;
  uint32_t pages = (*frame_buffer_size + 0xfff) >> 12;
  // Map whole MiBs
  pages = (pages + 0xff) & ~0xff;

  uint32_t *screen = Task_MapFrameBuffer( base_page, pages );

  { char const text[] = "Display ready ";
  Task_LogString( text, sizeof( text )-1 ); }
  Task_LogHexP( screen );
  Task_LogNewLine();

  for (;;) {
    queued_task client = Task_QueueWait( ws->queue );
    // There's only one thing that gets requested, so far, and that's
    // a "Wait for the display to be mapped at 0xc0000000" SWI
    Task_ReleaseTask( client.task_handle, 0 );
    Task_LogString( "Released the waiting task.", 0 );
    Task_LogNewLine();
  }
}

// Initialisation

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  // This in SVC mode, can't do things like wait for responses from
  // external hardware!
  workspace *ws = rma_claim( sizeof( workspace ) );
  *private = ws;

  ws->queue = Task_QueueCreate();

  swi_handlers handlers = { };
  for (int i = 0; i < 16; i++) {
    handlers.action[i].queue = ws->queue;
  }

  Task_RegisterSWIHandlers( &handlers );

  register void *start asm( "r0" ) = open_display;
  register uint32_t sp asm( "r1" ) = aligned_stack( ws + 1 );
  register workspace *r1 asm( "r2" ) = ws;

  register uint32_t handle asm( "r0" );

  // This module doesn't access hardware, so no need to spawn the task
  asm volatile ( // volatile because we ignore output
        "svc %[swi_create]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (handle)
    , "=r" (sp) // corrupted
    : [swi_create] "i" (OSTask_Create)
    , [swi_release] "i" (OSTask_ReleaseTask)
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

