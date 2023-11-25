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

#include "common.h"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

NO_start;
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

const char title[] = "BCM283XGPU";
const char help[] = "RasPi graphics\t0.01";

void spawn_gpu_manager( workspace *ws )
{
  register void *start asm( "r0" ) = gpu_task;
  register void *sp asm( "r1" ) = 0;
  register workspace *r1 asm( "r2" ) = ws;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );
}

void __attribute__(( noinline )) c_init( workspace **private )
{
  if (*private == 0) {
    *private = rma_claim( sizeof( workspace ) );
  }
  else {
    asm ( "udf 1" );
  }

  workspace *ws = *private;
  ws->queue = Task_QueueCreate();

  swi_handlers handlers = { };
  Task_RegisterSWIHandlers( &handlers );
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

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
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

typedef enum { HANDLER_PASS_ON, HANDLER_INTERCEPTED, HANDLER_FAILED } handled;

static void GraphicsV_ReadItems( uint32_t item, uint32_t *buffer, uint32_t len )
{
  switch (item) {
  case 4:
    {
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  default:
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  };
}

handled __attribute__(( noinline )) C_GraphicsV_handler( uint32_t *regs, struct workspace *workspace )
{
  union {
    struct {
      uint32_t code:16;
      uint32_t head:8;
      uint32_t driver:8;
    };
    uint32_t raw;
  } command = { .raw = regs[4] };

  if (command.driver != workspace->graphics_driver_id) {
    clear_VF();
    return HANDLER_PASS_ON;
  }

  switch (command.code) {
  case 0:
    break; // Null reason code for when vector has been claimed
  case 1:
    break; // VSync interrupt occurred 	BG 	SVC/IRQ
  case 2:
    break; // Set mode 	FG2 	SVC
  case 3:
    break; // Obsolete3 (was Set interlace) 	FG 	SVC
  case 4:
    break; // Set blank 	FG/BG 	SVC
  case 5:
    break; // Update pointer 	FG/BG 	SVC/IRQ
  case 6:
    break; // Set DAG 	FG/BG 	SVC/IRQ
  case 7:
    break; // Vet mode 	FG 	SVC
  case 8:  // Features 	FG 	SVC
    {
      regs[0] = 0x18; // No VSyncs, separate frame store, not variable frame store
      regs[1] = 0x20;
      regs[2] = 0;
      regs[4] = 0;
    }
    break;
  case 9:
    {
      regs[0] = workspace->fb_physical_address;
      regs[1] = 8 << 20; // FIXME
    }
    break;
  case 10:
    break; // Write palette entry 	FG/BG 	SVC/IRQ
  case 11:
    break; // Write palette entries 	FG/BG 	SVC/IRQ
  case 12:
    break; // Read palette entry 	FG 	SVC
  case 13:
    break; // Render 	FG 	SVC
  case 14:
    break; // IIC op 	FG 	SVC
  case 15:
    break; // Select head 	FG 	SVC
  case 16:
    break; // Select startup mode 	FG 	SVC
  case 17:
    break; // List pixel formats 	FG 	SVC
  case 18:
    GraphicsV_ReadItems( regs[0], (void*) regs[1], regs[2] );
    break; // Read info 	FG 	SVC
  case 19:
    break; // Vet mode 2 	FG 	SVC
  }

  regs[4] = 0; // Indicate to caller that call was intercepted

  return HANDLER_INTERCEPTED;
}

static void __attribute__(( naked )) GraphicsV_handler( char c )
{
  uint32_t *regs;
  asm ( "push { r0-r9, r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  asm ( "push {lr}" ); // Normal return address, to continue down the list

  register struct workspace *workspace asm( "r12" );
  handled result = C_GraphicsV_handler( regs, workspace );
  switch (result) {
  case HANDLER_FAILED: // Intercepted, but failed
  case HANDLER_INTERCEPTED:
    if (result == HANDLER_FAILED)
      set_VF();
    else
      clear_VF();
    asm ( "pop {lr}\n  pop { r0-r9, r12, pc }" );
    break;
  case HANDLER_PASS_ON:
    asm ( "pop {lr}\n  pop { r0-r9, r12 }\n  mov pc, lr" );
    break;
  }
}

