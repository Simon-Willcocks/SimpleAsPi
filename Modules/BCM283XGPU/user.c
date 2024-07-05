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

GPU volatile *const gpu = (void*) 0x1000;

void manage_gpu( uint32_t handle, workspace *ws )
{
  uint32_t gpu_page = 0x3f00b000 >> 12;
  Task_MapDevicePages( (uint32_t) gpu, gpu_page, 1 );
  for (;;) {
    Task_Yield();
  }
}


// TODO Take that code out of DynamicArea, and have another SWI for defining the screen, including the width and height...
static uint32_t *map_screen_into_memory( uint32_t address )
{
  register uint32_t r0 asm ( "r0" ) = 30;
  register uint32_t r1 asm ( "r1" ) = address;
  register uint32_t r2 asm ( "r2" ) = 8 << 20;  // Allows access to slightly more RAM than needed, FIXME (1920x1080x4 = 0x7e9000)
  // TODO Add a few more virtual lines, so that we're allocated the full 8MiB.
  register uint32_t *base asm ( "r1" );

  asm ( "svc %[OS_DynamicArea]" : "=r" (base) : [OS_DynamicArea] "i" (0x66), "r" (r0), "r" (r1), "r" (r2) : "lr", "cc" );

  return base;
}

// FIXME: Don't busy-wait for responses, have a module that handles
// GPU mailbox communications asynchronously!
uint32_t initialise_frame_buffer( struct workspace *workspace )
{
  GPU_mailbox volatile *mailbox = &workspace->gpu->mailbox[0];

  const int width = 1920;
  const int height = 1080;

  static const int space_to_claim = 26 * sizeof( uint32_t );
  static const uint32_t alignment = 2 << 20; // 2 MB aligned (more for long descriptor translation tables than short ones)

  dma_memory tag_memory = rma_claim_for_dma( space_to_claim, 16 );

  while (0 != (0xf & tag_memory.pa)) { tag_memory.pa++; tag_memory.va++; }

  uint32_t volatile *dma_tags = (void*) tag_memory.va;

  // Note: my initial sequence of tags, 0x00040001, 0x00048003, 0x00048004, 
  // 0x00048005, 0x00048006 didn't get a valid size value from QEMU.

  int index = 0;
  dma_tags[index++] = space_to_claim;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0x00048005;    // Colour depth
  dma_tags[index++] = 4;
  dma_tags[index++] = 0;
  dma_tags[index++] = 32;
  dma_tags[index++] = 0x00048006;    // Pixel order
  dma_tags[index++] = 4;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0;             // 0 = BGR, 1 = RGB
  dma_tags[index++] = 0x00048003;    // Set physical (display) width/height
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  dma_tags[index++] = width;
  dma_tags[index++] = height;
  dma_tags[index++] = 0x00048004;    // Set virtual (buffer) width/height
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  dma_tags[index++] = width;
  dma_tags[index++] = height + 13;    // Some hidden lines so that we are allocated whole MiB. FIXME for non-1080p
  // Despite a line of 1920 pixels being about 8k, the allocated amount varies enormously
  // 1088 results in 0x7f8000 (32KiB less than 8 MiB)
  // 1089 results in 0x816000 (88KiB more than 8 MiB)
  // 1093 is, by definition more than 8MB, so qemu, returning a closer size than the real hardware, will still work
  // It's safer to map in less than is allocated than more, since the ARM could corrupt GPU memory in the latter case
  // Mapping 0x800000 of the 0x816000 simply means 88KiB of memory won't be accessable by anyone.
  // Maybe we can use some of it for mouse pointers or something, as long as the GPU isn't used to clear the screen?
  dma_tags[index++] = 0x00040001;    // Allocate buffer
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  int buffer_tag = index;
  dma_tags[index++] = alignment;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0;             // End tag

  push_writes_to_cache();
  push_writes_out_of_cache( dma_tags, sizeof( dma_tags ) );

  uint32_t request = 8 | tag_memory.pa;

  while (dma_tags[buffer_tag] == alignment) {
    mailbox[1].value = request;
    push_writes_to_cache();

    uint32_t response;

    do {
      uint32_t countdown = 0x10000;
      while ((mailbox[0].status & (1 << 30)) != 0 && --countdown > 0) { asm volatile ( "dsb" ); } // Empty?
      if (countdown == 0) break;

      response = mailbox[0].value;
      if (response != request) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
    } while (response != request);
  }

  return (dma_tags[buffer_tag] & ~0xc0000000);
}

static uint32_t GraphicsV_DeviceNumber( char const *name )
{
  // OS_ScreenMode 65
  register uint32_t code asm( "r0" ) = 64;
  register uint32_t flags asm( "r1" ) = 0;
  register char const *driver_name asm( "r2" ) = name;
  register uint32_t allocated asm( "r0" );
  asm ( "svc 0x20065" : "=r" (allocated) : "r" (code), "r" (flags), "r" (driver_name) : "lr" );
  return allocated;
}

static void GraphicsV_DeviceReady( uint32_t number )
{
  // OS_ScreenMode 65
  register uint32_t code asm( "r0" ) = 65;
  register uint32_t driver_number asm( "r1" ) = number;
  asm ( "svc 0x20065" : : "r" (code), "r" (driver_number) : "lr" );
}

// Not static, or it won't be seen by inline assembler
void __attribute__(( noinline )) c_start_display( struct core_workspace *workspace )
{
  if (0 != workspace->graphics_driver_id) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );

  workspace->graphics_driver_id = GraphicsV_DeviceNumber( "BCM28xx" );

  {
    // This handler is not core-specific
    void *handler = GraphicsV_handler;
    register uint32_t vector asm( "r0" ) = 42;
    register void *routine asm( "r1" ) = handler;
    register struct workspace *handler_workspace asm( "r2" ) = workspace;
    asm ( "svc %[swi]" : : [swi] "i" (OS_Claim | Xbit), "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );
  }

  GraphicsV_DeviceReady( workspace->graphics_driver_id );
}

static void __attribute__(( naked )) start_display()
{
  asm ( "push { "C_CLOBBERED", lr }"
    "\n  mov r0, r12"
    "\n  bl c_start_display"
    "\n  pop { "C_CLOBBERED", pc }" );
}

static inline
uint64_t timer_now()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 0, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo) : : "memory"  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline
uint32_t timer_interrupt_time()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 2, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo)  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline
void timer_interrupt_at( uint64_t then )
{
  asm volatile ( "mcrr p15, 2, %[lo], %[hi], c14" : : [hi] "r" (then >> 32), [lo] "r" (0xffffffff & then) : "memory" );
}

static inline
void timer_set_countdown( int32_t timer )
{
  asm volatile ( "mcr p15, 0, %[t], c14, c2, 0" : : [t] "r" (timer) );
  // Clear interrupt and enable timer
  asm volatile ( "mcr p15, 0, %[config], c14, c2, 1" : : [config] "r" (1) );
}

static inline
int32_t timer_get_countdown()
{
  int32_t timer;
  asm volatile ( "mrc p15, 0, %[t], c14, c2, 0" : [t] "=r" (timer) );
  return timer;
}

static inline
uint32_t timer_status()
{
  uint32_t bits;
  asm volatile ( "mrc p15, 0, %[config], c14, c2, 1" : [config] "=r" (bits) );
  return bits;
}

static inline
bool timer_interrupt_active()
{
  return (timer_status() & 4) != 0;
}


// Returns with interrupts disabled for this core, enable the source
// and call Task_WaitForInterrupt asap.
static void disable_interrupts()
{
  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_IntOff)
      : "lr" );
}

static const int board_interrupt_sources = 64 + 12; // 64 GPU, 12 ARM peripherals (BCM2835-ARM-Peripherals.pdf, QA7)

uint32_t GetDebugPipe()
{
  register uint32_t pipe asm( "r0" );

  asm ( "svc %[swi]"
      : "=r" (pipe)
      : [swi] "i" (OSTask_GetDebugPipe)
      : "lr", "cc" );

  return pipe;
}

uint32_t NumberOfCores() // Returns 0 if legacy kernel
{
  register uint32_t count asm( "r0" );

  asm ( "svc %[swi]" 
    "\n  movvs r0, #0"
      : "=r" (count)
      : [swi] "i" (OSTask_NumberOfCores)
      : "lr", "cc" );

  return count;
}

void __attribute__(( noinline )) c_init( uint32_t this_core, uint32_t noc, struct workspace **private )
{
  uint32_t number_of_cores = NumberOfCores();

  assert( noc == number_of_cores );

  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;

  workspace->core_specific[this_core].pipe = GetDebugPipe();

  // Map this addresses into all cores
  workspace->gpu = map_device_page( 0x3f00b000 );

  workspace->gpio = map_device_page( 0x3f200000 );

#ifdef DEBUG__LED_ATTACHED
  if (first_entry) {
    led_init( workspace->gpio );
    led_blink( 0x04040404, workspace->gpio );
  }
#endif

  workspace->uart = map_device_page( 0x3f201000 );
  workspace->qa7 = map_device_page( 0x40000000 );

  workspace->uart->data = '0' + this_core;

  if (first_entry) {
    asm volatile ( "dsb" );

    workspace->fb_physical_address = initialise_frame_buffer( workspace );
  }

  workspace->frame_buffer = map_screen_into_memory( workspace->fb_physical_address );

show_word( this_core * (1920/4), 16, this_core*0x11111111, first_entry ? Red : Green, workspace ); 
show_word( this_core * (1920/4), 32, (uint32_t) workspace->gpio, first_entry ? Red : Green, workspace ); 
  QA7 volatile *qa7 = workspace->qa7;
show_word( this_core * (1920/4), 48, (uint32_t) &qa7->Core_write_clear[this_core], first_entry ? Red : Green, workspace ); 

  workspace->core_specific[this_core].shared = workspace;
  workspace->core_specific[this_core].queued = 0; // VDU code queue size, including character that started it filling
  workspace->core_specific[this_core].x = 0;
  workspace->core_specific[this_core].y = 0;
  for (int y = 0; y < 40; y++) {
    for (int x = 0; x < 60; x++) {
      workspace->core_specific[this_core].display[y][x] = ' ';
    }
  }

  GPU *gpu = workspace->gpu;

  if (first_entry) {
    // Involves calculation, don't assign directly to the register variable
    struct core_workspace *cws = &workspace->core_specific[this_core];

    register void *callback asm( "r0" ) = start_display;
    register struct core_workspace *ws asm( "r1" ) = cws;
    asm( "svc %[swi]" : : [swi] "i" (OS_AddCallBack | 0x20000), "r" (callback), "r" (ws) );
  }

show_word( this_core * (1920/4), 96, 0x11111111, first_entry ? Red : Green, workspace ); 

  clear_VF();
}

void __attribute__(( naked )) init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;

  // Move r12 into argument register
  asm volatile (
          "push { lr }"
      "\n  mov %[private_word], r12" : [private_word] "=r" (private) );

  c_init( this_core, number_of_cores, private );
  asm ( "pop { pc }" );
}

#include "Resources.h"

void register_files( uint32_t *regs )
{
  register void const *files asm ( "r0" ) = resources;
  register uint32_t service asm ( "r1" ) = regs[1];
  register uint32_t code asm ( "r2" ) = regs[2];
  register uint32_t workspace asm ( "r3" ) = regs[3];
  asm ( "mov lr, pc"
    "\n  mov pc, r2"
    :
    : "r" (files)
    , "r" (service)
    , "r" (code)
    , "r" (workspace)
    : "lr" );
}

void __attribute__(( naked )) service_call()
{
  asm ( "teq r1, #0x77"
    "\n  teqne r1, #0x50"
    "\n  teqne r1, #0x60"
    "\n  movne pc, lr" );


  // This is extremely minimal, and not all that efficient!
  // Object to mode changes. All of them.
  asm ( "teq r1, #0x77"
    "\n  moveq r1, #0"
    "\n  moveq r2, #0"
    "\n  moveq pc, lr" );

  asm ( "teq r1, #0x50"
    "\n  bne 0f"
    "\n  ldr r12, [r12]"
    "\n  mov r1, #0"
    "\n  adr r3, vidc_list"
/* VIDC List:
https://www.riscosopen.org/wiki/documentation/show/Service_ModeExtension
0 	3 (list format)
1 	Log2BPP mode variable
2 	Horizontal sync width (pixels)
3 	Horizontal back porch (pixels)
4 	Horizontal left border (pixels)
5 	Horizontal display size (pixels)
6 	Horizontal right border (pixels)
7 	Horizontal front porch (pixels)
8 	Vertical sync width (rasters)
9 	Vertical back porch (rasters)
10 	Vertical top border (rasters)
11 	Vertical display size (rasters)
12 	Vertical bottom border (rasters)
13 	Vertical front porch (rasters)
14 	Pixel rate (kHz)
15 	Sync/polarity flags:
Bit 0: Invert H sync
Bit 1: Invert V sync
Bit 2: Interlace flags (bits 3 and 4) specified, else kernel decides interlacing1
Bit 3: Interlace sync1
Bit 4: Interlace fields1
16+ 	Optional list of VIDC control list items (2 words each)
N 	-1 (terminator) 
 */
    "\nvidc_list: .word 3, 5, 0, 0, 0, 1920, 0, 0, 0, 0, 0, 1080, 0, 0, 8000, 0, -1"
    "\n  0:" );

  asm ( "teq r1, #0x60" // Service_ResourceFSStarting
    "\n  bne 0f"
    "\n  push { "C_CLOBBERED", lr }"
    "\n  mov r0, sp"
    "\n  bl register_files"
    "\n  pop { "C_CLOBBERED", pc }"
    "\n  0:" );

    asm (
    "\n  bkpt %[line]" : : [line] "i" (__LINE__) );
}

