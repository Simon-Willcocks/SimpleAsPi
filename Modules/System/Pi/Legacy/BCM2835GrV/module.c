/* Copyright 2024 Simon Willcocks
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

// This module handles the GraphicsV interface to the RISC OS kernel.
// The plan is to have a more general DisplayManager module and modules
// for individual platforms' displays.
// Bitmaps (including displays and regions) will be mapped into slot memory
// as required.

// For the time being, until the Wimp can be modified (maybe the Plot,
// Draw, and Font modules, too), provides one display on one screen at
// one resolution. Mapped at one, shared, location.

// There is horrible collusion between BCM2835Display, Legacy/memory.c,
// Task_MapFrameBuffer, and this module to map the one bitmap at 0xc0000000,
// globally.

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
//NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "BCM2835GrV";
const char help[] = "BCM2835GrV\t0.01 (" CREATION_DATE ")";

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

struct workspace {
  uint32_t graphics_driver_id;
  uint32_t stack[63];
};

static uint32_t GraphicsV_RegisterDriver( char const *name )
{
  // OS_ScreenMode 64 (Gets a number allocated.)
  register uint32_t code asm( "r0" ) = 64;
  register uint32_t flags asm( "r1" ) = 0;
  register char const *driver_name asm( "r2" ) = name;
  register uint32_t allocated asm( "r0" );
  asm ( "svc 0x20065" : "=r" (allocated) : "r" (code), "r" (flags), "r" (driver_name) : "lr" );
  return allocated;
}

static void GraphicsV_DeviceReady( uint32_t number )
{
  // OS_ScreenMode 65 (Willing to accept GraphicsV calls now.)
  register uint32_t code asm( "r0" ) = 65;
  register uint32_t driver_number asm( "r1" ) = number;
  asm ( "svc 0x20065" : : "r" (code), "r" (driver_number) : "lr" );
}

// 32 bpp -> 5 Log2BPP
static uint32_t const msb[] = { 1, 1920, 1080, 5, 60, -1 };

void open_display( uint32_t handle, workspace *ws )
{
  Task_LogString( "Opening BCM2835 display\n", 0 );

  register error_block const *error asm ( "r0" );
  asm volatile (
      "svc #0x220c0" // Wait for the display to be mapped at 0xc0000000
  "\n  movvc r0, #0"
      : "=r" (error) : : "cc" );

  if (error != 0) {
    Task_LogString( "BCM2835 display not opened\n", 0 );
    return;
  }

  GraphicsV_DeviceReady( ws->graphics_driver_id );

  Task_LogString( "GraphicsV device ready\n", 0 );

  Task_EndTask();
}

// GraphicsV interface

#define WriteS( s ) Task_LogString( s, sizeof( s ) - 1 )
#define Write0( s ) Task_LogString( s, 0 )
#define WriteNum( n ) Task_LogHex( n )
#define WriteSmallNum( n ) Task_LogSmallNumber( n )
#define Space Task_LogString( " ", 1 )

typedef enum { HANDLER_PASS_ON, HANDLER_INTERCEPTED, HANDLER_FAILED } handled;

static uint32_t GraphicsV_ReadItem( uint32_t item, uint32_t *buffer, uint32_t len )
{
  if (len == 0) return -4;

  switch (item) {
  case 4:
    {
      if (len >= 4) {
        buffer[0] = -1;
      }

      return len - 4;
    }
    break;
  default:
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  };

  return 0;
}

static inline
uint32_t get_pixel( int x, int y )
{
  return *(uint32_t*) (0xc0000000 + 4 * x + 4 * 1920 * (1080 - y));
}

static inline
void set_pixel( int x, int y, uint32_t p )
{
  *(uint32_t*) (0xc0000000 + 4 * x + 4 * 1920 * (1080 - y)) = p;
}

typedef struct {
  uint32_t sl;	// Source left edge
  uint32_t sb;	// Source bottom edge
  uint32_t dl;	// Dest left edge
  uint32_t db;	// Dest bottom edge
  uint32_t w;	// Width-1
  uint32_t h;	// Height-1
} copy_parms;

void RectCopy( copy_parms *const copy )
{
  Task_LogString( "Source left edge ", 0 ); Task_LogSmallNumber( copy->sl ); Task_LogNewLine();
  Task_LogString( "Source bottom edge ", 0 ); Task_LogSmallNumber( copy->sb ); Task_LogNewLine();
  Task_LogString( "Dest left edge ", 0 ); Task_LogSmallNumber( copy->dl ); Task_LogNewLine();
  Task_LogString( "Dest bottom edge ", 0 ); Task_LogSmallNumber( copy->db ); Task_LogNewLine();
  Task_LogString( "Width-1 ", 0 ); Task_LogSmallNumber( copy->w ); Task_LogNewLine();
  Task_LogString( "Height-1 ", 0 ); Task_LogSmallNumber( copy->h ); Task_LogNewLine();
  // TODO Hardware acceleration, or at least efficient implementation!
  if (copy->sb < copy->db) {
    // Go top to bottom, in case of overlap
    if (copy->sl < copy->dl) {
      // Go right to left, in case of overlap
      for (int y = copy->h; y >= 0; y--) {
        for (int x = copy->w; x >= 0; x--) {
          set_pixel( copy->dl + x, copy->db + y, get_pixel( copy->sl + x, copy->sb + y ) );
        }
      }
    }
    else {
      // Go left to right, in case of overlap
      for (int y = copy->h; y >= 0; y--) {
        for (int x = 0; x <= copy->w; x++) {
          set_pixel( copy->dl + x, copy->db + y, get_pixel( copy->sl + x, copy->sb + y ) );
        }
      }
    }
  }
  else {
    // Go bottom to top, in case of overlap
    if (copy->sl < copy->dl) {
      // Go right to left, in case of overlap
      for (int y = 0; y <= copy->h; y++) {
        for (int x = copy->w; x >= 0; x--) {
          set_pixel( copy->dl + x, copy->db + y, get_pixel( copy->sl + x, copy->sb + y ) );
        }
      }
    }
    else {
      // Go left to right, in case of overlap
      for (int y = 0; y <= copy->h; y++) {
        for (int x = 0; x <= copy->w; x++) {
          set_pixel( copy->dl + x, copy->db + y, get_pixel( copy->sl + x, copy->sb + y ) );
        }
      }
    }
  }
}

typedef struct {
  uint32_t or_mask;
  uint32_t eor_mask;
} ecf_line;

typedef struct {
  ecf_line line[8];
} ECF;

typedef struct {
  uint32_t left;
  uint32_t top;
  uint32_t right;
  uint32_t bottom;
  ECF      *ecf;
} fill_parms;

void RectFill( fill_parms *const fill )
{
  ECF const *ecf = fill->ecf;

  char const text[] = " ECF\n";
  Task_LogString( text, sizeof( text )-1 );

  for (int i = 0; i < 8; i++) {
    Task_LogHex( ecf->line[i].or_mask );
    Task_LogHex( ecf->line[i].eor_mask );
    Task_LogNewLine();
  }

  if (ecf->line[0].or_mask == 0xffffffff) {
    uint32_t c = ~ecf->line[0].eor_mask;
    for (int y = fill->bottom; y <= fill->top; y++) {
      for (int x = fill->left; x <= fill->right; x++) {
        set_pixel( x, y, c );
      }
    }
  }
  else {
    for (int y = fill->bottom; y <= fill->top; y++) {
      for (int x = fill->left; x <= fill->right; x++) {
        set_pixel( x, y, (get_pixel( x, y ) | ecf->line[0].or_mask) ^ ecf->line[0].eor_mask );
      }
    }
  }
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

#if 0
#define VERBOSE
  Write0( "GraphicsV " );
  WriteSmallNum( command.raw );
  Task_LogNewLine();
#endif

  switch (command.code) {
  case 0:
    break; // Null reason code for when vector has been claimed
  case 1:
    WriteS( "VSync interrupt occurred " );
    break; // VSync interrupt occurred 	BG 	SVC/IRQ
  case 2:
    WriteS( "Set mode " );
    break; // Set mode 	FG2 	SVC
  case 3:
    WriteS( "Obsolete3 (was Set interlace) " );
    break; // Obsolete3 (was Set interlace) 	FG 	SVC
  case 4:
    WriteS( "Set blank " );
    break; // Set blank 	FG/BG 	SVC
  case 5:
    WriteS( "Update pointer " );
    break; // Update pointer 	FG/BG 	SVC/IRQ
  case 6:
    WriteS( "Set DAG " ); WriteNum( regs[0] ); WriteS( " " ); WriteNum( regs[1] );
    // TODO: hardware scroll, etc.?
    regs[4] = 0; // Pretend we've done something with this...
    break; // Set DAG 	FG/BG 	SVC/IRQ
  case 7:
    WriteS( "Vet mode " );
    WriteNum( regs[0] ); WriteS( " -> " );
    uint32_t *list = (void*) regs[0];
    WriteNum( list[0] ); WriteS( " " );
    WriteNum( list[1] ); WriteS( " " );
    WriteNum( list[2] ); WriteS( " " );
    WriteNum( list[3] ); WriteS( " " );
    WriteNum( list[4] ); WriteS( " " );
    WriteNum( list[5] ); WriteS( " " );
    WriteNum( list[6] ); WriteS( " " );
    WriteNum( list[7] ); WriteS( " " );
    WriteNum( list[8] ); WriteS( " " );
    WriteNum( list[9] ); WriteS( " " );
    WriteNum( list[10] ); WriteS( " " );
    WriteNum( list[11] ); Task_LogNewLine();
    if (list[0] != 3
     || list[1] != 5
     || list[5] != 1920
     || list[11] != 1080) {
      WriteS( "Unsupported mode!\n" );
      for (;;) { Task_Yield(); }
      asm( "bkpt 1" );
    }

    regs[0] = 0;
    break; // Vet mode 	FG 	SVC
  case 8:  // Features 	FG 	SVC
    {
      // No VSyncs, separate frame store, not variable frame store,
      regs[0] = 0x38;
      // Mask of supported pixel formats
      regs[1] = (1 << 5); // 32bpp RGB (C16M LTBGR)
      // Display buffer alignment requirement
      regs[2] = (1 << 20);
    }
    break;
  case 9:
    WriteS( "Framestore information " ); // Framestore information 	FG 	SVC
    {
      regs[0] = 0xfb000000; // Should cause a memory fault if it ever gets used
      // FIXME
      regs[1] = msb[1] * msb[2] * (1 << msb[3]) / 8;
    }
    break;
  case 10:
    // WriteNum( regs[0] ); Space; 
    // WriteNum( regs[1] ); Space; 
    // WriteNum( regs[2] ); Space; 
    // WriteS( "Write palette entry, ignored for now " );
    break; // Write palette entry 	FG/BG 	SVC/IRQ
  case 11:
    WriteS( "Write palette entries " );
    break; // Write palette entries 	FG/BG 	SVC/IRQ
  case 12:
    WriteS( "Read palette entry " );
    break; // Read palette entry 	FG 	SVC
  case 13:
    switch (regs[1]) {
    case 0: // Nop
      break;
    case 1:
      RectCopy( (void*) regs[2] );
      break;
    case 2:
      RectFill( (void*) regs[2] );
      break;
    default:
      break;
    }
    break; // Render 	FG 	SVC
  case 14:
    WriteS( "IIC op " );
    break; // IIC op 	FG 	SVC
  case 15:
    WriteS( "Select head " );
    break; // Select head 	FG 	SVC
  case 16:
    WriteS( "Select startup mode " );
    asm ( "bkpt 89" );
    break; // Select startup mode 	FG 	SVC
  case 17:
    WriteS( "List pixel formats " );
    break; // List pixel formats 	FG 	SVC
  case 18:
    regs[2] = GraphicsV_ReadItem( regs[0], (void*) regs[1], regs[2] );
    regs[4] = 0; // https://www.riscosopen.org/wiki/documentation/show/GraphicsV%2018
    break; // Read info 	FG 	SVC
  case 19:
    {
    WriteS( "Vet mode 2 " );
    WriteNum( regs[2] ); WriteS( ", " );
    WriteNum( regs[0] ); WriteS( " -> " );
    uint32_t *list = (void*) regs[0];
    WriteNum( list[0] ); WriteS( " " );
    WriteNum( list[1] ); WriteS( " " );
    WriteNum( list[2] ); WriteS( " " );
    WriteNum( list[3] ); WriteS( " " );
    WriteNum( list[4] ); WriteS( " " );
    WriteNum( list[5] ); WriteS( " " );
    WriteNum( list[6] ); WriteS( " " );
    WriteNum( list[7] ); WriteS( " " );
    WriteNum( list[8] ); WriteS( " " );
    WriteNum( list[9] ); WriteS( " " );
    WriteNum( list[10] ); WriteS( " " );
    WriteNum( list[11] ); Task_LogNewLine();
    }
    regs[0] = 3;
    regs[1] = 2 << 20;
    regs[2] = 0;
    // Only change 3 and 5 if regs[0] == 2
    break; // Vet mode 2 	FG 	SVC
  }

#ifdef VERBOSE
  Task_LogNewLine();
#endif

  regs[4] = 0; // Indicate to caller that call was intercepted

  return HANDLER_INTERCEPTED;
}

static void __attribute__(( naked )) GraphicsV_handler( char c )
{
  uint32_t *regs;
  asm ( "udf 5" );

  asm ( "push { r0-r9, r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  asm ( "push {lr}" ); // Normal return address, to continue down the list
  asm ( "udf #88" ); // FIXME remove

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
  default: asm ( "bkpt 3" );
  }
}

void __attribute__(( noinline )) C_service_call( uint32_t *regs, struct workspace *workspace )
{
  //Task_LogString( "Service call ", 0 );
  //Task_LogHex( regs[1] );
  //Task_LogNewLine();
  switch (regs[1]) {
  case 0x4d: // Service_PreModeChange
    {
    // FIXME: More modes!
    regs[0] = 0; // No error
    // Do NOT claim the call, this is Case 4 in the documentation
    regs[2] = (uint32_t) msb;
    }
    break;
  case 0x50: // Service_ModeExtension
    {
      if (regs[2] < 256) asm( "bkpt 1" ); // Mode number
      if ((regs[2] & 1) != 0) asm( "bkpt 1" ); // Sprite mode word

      uint32_t *spec = (void*) regs[2];
      if (spec[1] != 1920
       || spec[2] != 1080
       || spec[3] != 5) asm( "bkpt 1" );

      Task_LogString( "Service_ModeExtension ", 0 );
      Task_LogHex( regs[2] );
      Task_LogString( " ", 1 );
      Task_LogHex( regs[3] );
      Task_LogString( " ", 1 );
      Task_LogHex( regs[4] );
      Task_LogString( " ", 1 );
      Task_LogHex( regs[5] );
      Task_LogNewLine();
      regs[1] = 0;
      regs[4] = 0;
      static uint32_t const vidc_list[] = {
        3,      // version
        5,      // log2 bpp

        0, // Horizontal sync width (pixels)
        0, // Horizontal back porch (pixels)
        0, // Horizontal left border (pixels)
        1920, // Horizontal display size (pixels)
        0, // Horizontal right border (pixels)
        0, // Horizontal front porch (pixels)
        0, // Vertical sync width (rasters)
        0, // Vertical back porch (rasters)
        0, // Vertical top border (rasters)
        1080, // Vertical display size (rasters)
        0, // Vertical bottom border (rasters)
        0, // Vertical front porch (rasters)
        0, // Pixel rate (kHz)
        0, // Sync/polarity flags:
        -1
      };
      regs[3] = (uint32_t) vidc_list;
    }
    break;
  case 0xde: // Service_DisplayChanged (info only)
    {
      Task_LogString( "Service_DisplayChanged ", 0 );
      Task_LogHex( regs[0] );
      Task_LogString( " ", 1 );
      Task_LogHex( regs[2] );
      Task_LogString( " ", 1 );
      Task_LogHex( regs[3] );
      Task_LogNewLine();
    }
    break;
  case 0xdf: // Service_DisplayStatus information only
    break;
  default:
    break;
  }
}

static void __attribute__(( naked )) service_call()
{
  uint32_t *regs;
  asm ( "push { r0-r9, r12, lr }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  struct workspace *workspace = (void*) regs[10];
  C_service_call( regs, workspace );

  asm ( "pop { r0-r9, r12, pc }" );

  // Avoid unused function warning
  asm ( "" : : "m" (service_call) );
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

  ws->graphics_driver_id = GraphicsV_RegisterDriver( "BCM28xx" );

  {
    // Note, this looks a little odd. Possibly written this way to
    // avoid problems with absolute addresses. Possibly not needed!
    // FIXME
    void *handler = GraphicsV_handler;
    register uint32_t vector asm( "r0" ) = 42;
    register void *routine asm( "r1" ) = handler;
    register struct workspace *handler_workspace asm( "r2" ) = ws;
    asm ( "svc %[swi]" : : [swi] "i" (OS_Claim | Xbit), "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );
  }

  WriteS( "Obtained GraphicsV" );
  Task_LogNewLine();

  register void *start asm( "r0" ) = open_display;
  register uint32_t sp asm( "r1" ) = aligned_stack( ws + 1 );
  register workspace *r1 asm( "r2" ) = ws;

  register uint32_t handle asm( "r0" );
  asm volatile (
        "svc %[swi_create]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (sp)
    , "=r" (handle)
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
