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
#include "ostaskops.h"
#include "bcm_uart.h"



typedef struct workspace workspace;

struct workspace {
  uint32_t lock;
  uint32_t output_pipe;
  uint32_t stack[60];
};

#define MODULE_CHUNK "0"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

//NO_start;
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

const char title[] = "LogToScreen";
const char help[] = "LogToScreen\t0.01 (" CREATION_DATE ")";

UART volatile *const uart  = (void*) 0x1000;

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}


#include "BBCFont.h"

typedef struct window {
  char display[24][60];
  uint8_t bottom_row;
  uint8_t index;
  uint32_t stack[32];
} window;

uint32_t *const screen = (void*) 0xc0000000;

static inline
void newline( window *w )
{
  if (w->bottom_row == 0)
    w->bottom_row = 23;
  else
    --w->bottom_row;
  for (int x = 0; x < number_of( w->display[0] ); x++) {
    w->display[w->bottom_row][x] = ' ';
  }
  w->index = 0;
}

static inline
void add_char( window *w, char c )
{
  if (c == '\r') return;
  if (c == '\n') {
    newline( w );
    return;
  }
  if (w->index == 59) {
    newline( w );
  }
  if (c == '\t') {
    w->index = (w->index + 8) & ~7;
    if (w->index > 59) newline( w );
    return;
  }
  // Actual visible character! (if 32 <= c < 128)
  w->display[w->bottom_row][w->index++] = c;
}

static inline
void add_to_display( window *w, char const *string, uint32_t len )
{
  for (int i = 0; i < len; i++) add_char( w, string[i] );
}

static inline
void show_char( uint32_t *topleft, int x, int y, char c, uint32_t fg )
{
  if (c < 32 || c >= 128) c = '?'; // asm ( "bkpt 77" ); // c = 32;
  uint8_t *row = HardFont[c-32];
  uint32_t *cell = topleft + 1920 * y * 8 + 8 * x;
  for (int i = 0; i < 8; i++) {
    for (int j = 7; j >= 0; j--) {
      *cell = (0 != (((*row) >> j) & 1)) ? fg : 0;
      cell++;
    }
    row++;
    cell += 1920 - 8;
  }
}

static inline
void show_word( uint32_t *topleft, int x, int y, uint32_t word, uint32_t fg )
{
  // NB. Do not use static char const *hex =...
  char const hex[] = "0123456789abcdef";

  for (int n = 0; n < 8; n++) {
    char c = hex[word & 0xf];
    word = word >> 4;
    show_char( topleft, x + (7-n), y, c, fg );
  }
}

void show_display( window *w, uint32_t *topleft )
{
  int line = w->bottom_row;
  for (int y = 0; y < 24; y++) {
    for (int x = 0; x < 59; x++) {
      show_char( topleft, x, 23-y, w->display[line][x], 0x00ffff00 );
    }
    if (++line == 24) line = 0;
  }
}

void core_debug_task( uint32_t handle, int core, window *w, uint32_t pipe )
{
  uint32_t *topleft = screen + 19200 + core * (1920 / 4) + 4;
  w->bottom_row = 0;
  w->index = 0;

  for (int i = 0; i < 59*8; i++) topleft[i] = 0xffff0000;
  for (int i = 0; i < 24; i++) {
    newline( w );
    for (int j = 0; j < i; j++) add_char( w, ' ' );
    add_char( w, 'A' + i );
  }

  add_to_display( w, "Hello", 5 );
  add_to_display( w, "\nWorld", 6 );
  show_display( w, topleft );

  Task_FlushCache( screen, 0x800000 );

  Task_LogString( "Waiting for log ", 16 );
  Task_LogHex( pipe );
  Task_LogString( "\n", 1 );

  for (;;) {
    PipeSpace data = PipeOp_WaitForData( pipe, 1 );
    while (data.available != 0) {
      add_to_display( w, data.location, data.available );
      data = PipeOp_DataConsumed( pipe, data.available );
    }

    show_display( w, topleft );
    Task_FlushCache( screen, 0x800000 );
  }
}


#ifdef DEBUG__BREAK_LOG_TO_SCREEN
static inline uint32_t rgb( int col )
{
  asm ( "" : : "m" (memcpy) ); // Force a non-inline copy of memcpy
  uint32_t cols[] = {
#else
static inline uint32_t rgb( int col )
{
  // Doesn't use memcpy:
  static uint32_t const cols[] = {
#endif
                      0x00000000,
                      0x00ff0000, // Red.
                      0x0000ff00, // Green.
                      0x0000ffff, // Cyan
                      0x00ff00ff, // Magenta
                      0x00ffff00, // Yellow
                      0x00ffffff, // White
                      0x000000ff, // Blue. (not very bright!)
                      0x0000ff80, 
                      0x00ff0080, 
                      0x0080ff00, 
                      0x00ff8080, 
                      0x0080ff80, 
                      0x008080ff, 
                      0x00ffff80, 
                      0x0080ffff, 
                      0x00ff80ff, 
                      0x00808080 };
  return cols[col];
}

void start_log( uint32_t handle, workspace *ws )
{
  // Wait until screen mapped. Assuming HD at 0xc0000000.
  asm ( "swi 0x20c0" );

  core_info cores = Task_Cores();

  for (int i = cores.total - 1; i >= 0; i--) {
    window *win = rma_claim( sizeof( window ) );

    // Note: The above rma_claim may (probably will) result in the core
    // we're running on to change.
    // Therefore we have to run it before switching (or switch more than
    // once).
  
    Task_SwitchToCore( i );

    uint32_t pipe = Task_GetLogPipe();

    Task_LogHex( pipe ); Task_LogNewLine();
    
    if (pipe != 0) {
      // Just use this task for core 0
      if (i == 0) core_debug_task( handle, 0, win, pipe );

      register void *start asm( "r0" ) = core_debug_task;
      register uint32_t sp asm( "r1" ) = aligned_stack( win + 1 );
      register uint32_t r1 asm( "r2" ) = i;
      register window *r2 asm( "r3" ) = win;
      register uint32_t r3 asm( "r4" ) = pipe;

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
        , "r" (r2)
        , "r" (r3)
        : "lr", "cc" );
    }
    else asm ( "bkpt 6" );
  }
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  workspace *ws = rma_claim( sizeof( workspace ) );
  *private = ws;
  ws->lock = 0;
  ws->output_pipe = 0;

  register void *start asm( "r0" ) = start_log;
  register uint32_t sp asm( "r1" ) = aligned_stack( ws + 1 );
  register workspace *r1 asm( "r2" ) = ws;
  register uint32_t handle asm( "r0" );
  asm volatile (
        "svc %[swi_spawn]"
    "\n  mov r1, #0"
    "\n  svc %[swi_release]"
    : "=r" (sp)
    , "=r" (handle)
    : [swi_spawn] "i" (OSTask_Spawn)
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

void __attribute__(( noreturn )) Logging()
{
  register uint32_t pin asm( "r0" ) = 27; // 22 green 27 orange
  register uint32_t on asm( "r1" ) = 200;
  register uint32_t off asm( "r2" ) = 100;
  asm ( "svc 0x1040" : : "r" (pin), "r" (on), "r" (off) );

  for (;;) {
    Task_LogString( "Loggy ", 6 );
    Task_Sleep( 100 );
  }
}

void start()
{
  // Running in usr32 mode, no stack
  asm ( "mov r0, #0x9000"
    "\n  svc %[settop]"
    "\n  mov sp, r0"
    :
    : [settop] "i" (OSTask_AppMemoryTop)
    : "r0" );

  Logging();
}
