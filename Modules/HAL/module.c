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

#include "CK_types.h"
#include "qa7.h"
#include "bcm_gpio.h"
#include "bcm_gpu.h"

typedef struct workspace workspace;

struct workspace {
  GPU *gpu;
  QA7 *qa7;
  uint32_t last_reported_irq;
};

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

#include "ostaskops.h"

//NO_start;
//NO_init;
NO_finalise;
//NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "HAL";
const char help[] = "RasPi3 HAL\t0.01";

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

int next_active( struct workspace *workspace, uint32_t core )
{
  // This is where we will use the hardware to identify which devices have
  // tried to interrupt the processor.
  QA7 volatile *qa7 = workspace->qa7;

  memory_read_barrier();

  // Source is: QA7 core interrupt source; bit 8 is GPU interrupt, bit 0 is physical timer

  uint32_t source = qa7->Core_IRQ_Source[core];
  bool found = false;
  GPU *gpu = workspace->gpu;

  // TODO is the basic_pending register still a thing?
  // TODO ignore interrupts that come from the GPU! They may be masked, but do they still show as pending?

  memory_read_barrier();

  // There are a few speedups possible
  //  e.g. test bits by seeing if (int32_t) (source << (32-irq)) is -ve, or zero (skip the rest of the bits)
  //  count leading zeros instruction...

  int last_reported_irq = workspace->last_reported_irq;
  int irq = last_reported_irq;
  bool last_possibility = false;

  do {
    irq++;
    last_possibility = (irq == last_reported_irq);
    // WriteNum( irq ); Space;
    if (irq >= 0 && irq < 64) {
      if (0 == (source & (1 << 8))) {
        // Nothing from GPU, don't need to check anything under 64
        irq = 63;
      }
      else {
        uint32_t pending;
        if (irq < 32) {
          pending = gpu->pending1;
        }
        else {
          pending = gpu->pending2;
        }
        // We only get here with irq & 0x1f non-zero if the previous reported was in this range
        assert( (0 != (irq & 0x1f)) == (irq == last_reported_irq+1) );
        pending = pending >> (irq & 0x1f);
        while (pending != 0 && 0 == (pending & 1)) {
          irq++;
          pending = pending >> 1;
        }
        found = pending != 0;
        if (!found) {
          irq = irq | 0x1f;
        }
        // Next time round will be in next 32-bit chunk
        assert( found || 0x1f == (irq & 0x1f) );
      }
    }
    else if (irq == 72) {
      // Covered by 0..63
    }
    else if (irq < 76) {
      // 64 CNTPSIRQ
      // 65 CNTPNSIRQ
      // 66 CNTHPIRQ
      // 67 CNTVIRQ
      // 68 Mailbox 0
      // 69 Mailbox 1
      // 70 Mailbox 2
      // 71 Mailbox 3
      // 72 (GPU, be more specific, see above)
      // 73 PMU 
      // 74 AXI outstanding (core 0 only)
      // 75 Local timer 
      found = (0 != (source & (1 << (irq & 0x1f))));
    }
    else
      irq = -1; // Wrap around to 0 on the next loop

    // Check each possible source once, but stop if found
  } while (!found && !last_possibility);

  if (found) {
    workspace->last_reported_irq = irq;

    return irq;
  }
  else {
    return -1;
  }
}

static const int board_interrupt_sources = 64 + 12; // 64 GPU, 12 ARM peripherals (BCM2835-ARM-Peripherals.pdf, QA7)

void __attribute__(( noinline )) c_init( workspace **private )
{
  if (*private == 0) {
    *private = rma_claim( sizeof( workspace ) );
  }
  else {
    asm ( "udf 1" );
  }

  workspace *ws = *private;

  register uint32_t number asm ( "r0" ) = board_interrupt_sources;
  register int (*next)( workspace *, uint32_t ) asm ( "r1" ) = next_active;
  register workspace *wkspce asm ( "r2" ) = ws;
  register uint32_t dev1 asm ( "r3" ) = 0x40000000 >> 12;
  register uint32_t dev2 asm ( "r4" ) = 0x3f00b000 >> 12;
  register uint32_t devstop asm ( "r5" ) = 0xffffffff;

  asm ( "svc %[swi]"
    : "=r" (dev1)
    , "=r" (dev2)
    : [swi] "i" (OSTask_RegisterInterrupts)
    , "r" (number)
    , "r" (next)
    , "r" (wkspce)
    , "r" (dev1)
    , "r" (dev2)
    , "r" (devstop)
    : "lr", "cc" );

  ws->qa7 = (void*) dev1;
  ws->gpu = (void*) dev2;

  ws->last_reported_irq = 0;
}

void __attribute__(( naked )) init()
{
  struct workspace **private;

  // Move r12 into argument register
  asm volatile (
          "push { lr }"
      "\n  mov %[private_word], r12" : [private_word] "=r" (private) );

  c_init( private );
  asm ( "pop { pc }" );
}


void __attribute__(( noreturn )) boot( char const *cmd, workspace *ws )
{
  char command[64];
  char *mod;
  // -fPIE does insane stuff with strings, copying the rodata string
  // to the stack so that I can copy it onto the stack from the wrong
  // address...
  // I think the offset tables are supposed to be fixed up by the standard
  // library (which isn't going to work in ROM modules, is it?).
  asm ( "mov %[m], #0"
    "\n0:"
    "\n  ldrb lr, [%[s], %[m]]"
    "\n  cmp lr, #0"
    "\n  strb lr, [%[d], %[m]]"
    "\n  addne %[m], %[m], #1"
    "\n  bne 0b"
    "\n  add %[m], %[d], %[m]"
    : [m] "=&r" (mod)
    : [s] "r" ("System:Modules.")
    , [d] "r" (command)
    : "lr" );

  char const *s = 
    "BCM283XGPU\0"
    "FileSwitch\0"
    "ResourceFS\0"
    "TerritoryManager\0"
    "Messages\0"
    "MessageTrans\0"
    "UK\0"
    "WindowManager\0"
    "TaskManager\0"
    "Desktop\0"
    "SharedRISC_OSLib\0"
    "BASIC105\0"
    "BASIC64\0"
    "BASICVFP\0"
    "BlendTable\0"
    "BufferManager\0"
    "ColourTrans\0"
    "DeviceFS\0";

  while (*s != '\0') {
    char *p = mod;
    do {
      *p++ = *s++;
    } while (*s != '\0');
    *p = '\0';
    s++;

    register uint32_t load asm ( "r0" ) = 1; // RMLoad
    register char const *module asm ( "r1" ) = command;

    asm ( "svc %[swi]" : : [swi] "i" (OS_Module), "r" (load), "r" (module) );
  }

  for (;;) asm ( "udf 8" );

  __builtin_unreachable();
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

void __attribute__(( naked )) start( char const *command )
{
  register workspace *ws asm ( "r12" );

  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  boot( command, ws );

  __builtin_unreachable();
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

/*
  asm ( "teq r1, #0x60" // Service_ResourceFSStarting
    "\n  bne 0f"
    "\n  push { "C_CLOBBERED", lr }"
    "\n  mov r0, sp"
    "\n  bl register_files"
    "\n  pop { "C_CLOBBERED", pc }"
    "\n  0:" );
*/
    asm (
    "\n  bkpt %[line]" : : [line] "i" (__LINE__) );
}

