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

typedef struct workspace workspace;

struct workspace {
  uint32_t lock;
};

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

#include "ostaskops.h"

enum XWIMPOP {
    XWimp_Initialise = 0x600c0,
    XWimp_CreateWindow,
    XWimp_CreateIcon,
    XWimp_DeleteWindow,
    XWimp_DeleteIcon,
    XWimp_OpenWindow,
    XWimp_CloseWindow,
    XWimp_Poll,
    XWimp_RedrawWindow,
    XWimp_UpdateWindow,
    XWimp_GetRectangle,
    XWimp_GetWindowState,
    XWimp_GetWindowInfo,
    XWimp_SetIconState,
    XWimp_GetIconState,
    XWimp_GetPointerInfo,
    XWimp_DragBox,
    XWimp_ForceRedraw,
    XWimp_SetCaretPosition,
    XWimp_GetCaretPosition,
    XWimp_CreateMenu,
    XWimp_DecodeMenu,
    XWimp_WhichIcon,
    XWimp_SetExtent,
    XWimp_SetPointerShape,
    XWimp_OpenTemplate,
    XWimp_CloseTemplate,
    XWimp_LoadTemplate,
    XWimp_ProcessKey,
    XWimp_CloseDown,
    XWimp_StartTask,
    XWimp_ReportError,
    XWimp_GetWindowOutline,
    XWimp_PollIdle,
    XWimp_PlotIcon,
    XWimp_SetMode,
    XWimp_SetPalette,
    XWimp_ReadPalette,
    XWimp_SetColour,
    XWimp_SendMessage,
    XWimp_CreateSubMenu,
    XWimp_SpriteOp,
    XWimp_BaseOfSprites,
    XWimp_BlockCopy,
    XWimp_SlotSize,
    XWimp_ReadPixTrans,
    XWimp_ClaimFreeMemory,
    XWimp_CommandWindow,
    XWimp_TextColour,
    XWimp_TransferBlock,
    XWimp_ReadSysInfo,
    XWimp_SetFontColours,
    XWimp_GetMenuState,
    XWimp_RegisterFilter,
    XWimp_AddMessages,
    XWimp_RemoveMessages,
    XWimp_SetColourMapping,
    XWimp_TextOp,
    XWimp_SetWatchdogState,
    XWimp_Extend,
    XWimp_ResizeIcon };

enum WIMPEVT {
    Null_Reason_Code = 0,
    Redraw_Window_Request,
    Open_Window_Request,
    Close_Window_Request,
    Pointer_Leaving_Window,
    Pointer_Entering_Window,
    Mouse_Click,
    User_Drag_Box,
    Key_Pressed,
    Menu_Selection,
    Scroll_Request,
    Lose_Caret,
    Gain_Caret,
    Poll_word_non_zero,
    // 14-16 reserved,
    User_Message = 17,
    User_Message_Recorded,
    User_Message_Acknowledge };

//NO_start;
NO_init;
NO_finalise;
NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "HAL";
const char help[] = "RasPi3 HAL\t0.01 (" CREATION_DATE ")";

static inline void Plot( uint32_t code, uint32_t x, uint32_t y )
{
  register uint32_t c asm( "r0" ) = code;
  register uint32_t xpos asm( "r1" ) = x;
  register uint32_t ypos asm( "r2" ) = y;
  asm volatile ( "svc %[swi]"
    : "=r" (c)
    , "=r" (xpos)
    , "=r" (ypos)
    : [swi] "i" (OS_Plot)
    , "r" (c)
    , "r" (xpos)
    , "r" (ypos) );
}

void __attribute__(( noreturn )) boot( char const *cmd, workspace *ws )
{
  static char const base[] = "System:Modules.";
  char command[128];
  char *mod = command;
  char const *b = base;
  while (*b != 0) {
    *mod++ = *b++;
  }

  // INITIAL_MODULES provided by build script.
  // Must be a single string consiting of nul-terminated module
  // names, e.g. "QA7\0BCM_GPIO\0". (That final null is important,
  // it ensures the string is double-nul-terminated.)
  char const *s = INITIAL_MODULES;

  while (*s != '\0') {
    char *p = mod;
    do {
      *p++ = *s++;
    } while (*s >= ' ');
    *p++ = '\n'; // Terminator
    s++;

    Task_LogString( command, p - command );
    Task_Yield();

    register uint32_t load asm ( "r0" ) = 1; // RMLoad
    register char const *module asm ( "r1" ) = command;
    register error_block *error asm ( "r0" );

    asm ( "svc %[swi]"
      "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OS_Module), "r" (load), "r" (module)
      : "cc", "memory" );
    // "memory" is required because the SWI accesses memory
    // (the module name). Without it, the final *p = '\n'; may
    // be delayed until after the SWI is called.

    if (error != 0) {
      asm ( "udf 7" );
    }

    // In case the initialisation kicked off some tasks, let them run!
    // Legacy modules might have set callbacks, they'll have been run
    // by the time we get here. (Modules are not permitted to call
    // Yield in their svc mode initialisation code.)

    // Task_Yield(); Moved to after the LogString and after the loop
  }

  Task_Yield();

if (0) {
Task_Sleep( 1 );
char const cmd[] = "Modules";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}

      if (0) {
        // Dealt with by OS_Byte
        char const cmd[] = "Set Wimp$Font Homerton.Medium";
        register char const *s asm( "r0" ) = cmd;
        asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
      }

if (0) {
char const cmd[] = "cat Resources:$.Resources.Wimp";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
// Task_Sleep( 4 );
}

if (1) {
Task_Sleep( 1 );
char const cmd[] = "Show *";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (Xbit | OS_CLI), "r" (s) );
}

#define Log(s) do { static char const text[] = s; Task_LogString( text, sizeof( text )-1 ); } while (false)
#define LogNL Task_LogNewLine()

if (0) {
//asm ( "svc 0x66666" );
Task_Sleep( 1 );
char const cmd[] = "Info Switcher:Templates";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (Xbit | OS_CLI), "r" (s) );
Task_Sleep( 10 );
Log( "Open\n" );
{
static char const file[] = "Switcher:Templates";
{
  register char const *f asm ( "r1" ) = file;
  asm ( "svc %[swi]" : : [swi] "i" (0x600d9), "r" (f) );
}
Log( "Scan\n" );
Task_Yield();
{
  void *context = 0;
  char buffer[1000];
  do {
    char name[12];
    name[0] = '*';
    name[1] = '\0';

    register uint32_t b asm ( "r1" ) = 0; // Request size
    register char *i asm ( "r2" ) = buffer; // indirect ws
    register char *e asm ( "r3" ) = buffer + sizeof( buffer ); // past end
    register uint32_t f asm ( "r4" ) = -1; // No fonts
    register char const *n asm ( "r5" ) = name;
    register void *c asm ( "r6" ) = context;

    asm ( "svc %[swi]"
        : "=r" (c)
        , "=r" (i)
        , "=r" (n)
        : [swi] "i" (0x600db)
        , "r" (b)
        , "r" (i)
        , "r" (e)
        , "r" (f)
        , "r" (n)
        , "r" (c)
        , "r" (name)
        : "r0", "memory" );
    context = c;
    if (context != 0) {
      int len = 0;
      while (n[len] > ' ' && len < 12) len++;
      Task_LogString( n, len );
      Task_LogNewLine();
    }
  } while (context != 0);
  char text[] = "End of templates.\n";
  Task_LogString( text, sizeof( text )-1 );
}
 
Log( "Close\n" );
{
  asm ( "svc %[swi]" : : [swi] "i" (0x600da) : "r0" );
}
Log( "Closed\n" );
}
}

  static char const entering[] = "Entering default language: " DEFAULT_LANGUAGE " in ";
  Task_LogString( entering, sizeof( entering ) - 1 );

for (int i = 3; i > 0; i--) { Task_LogSmallNumber( i ); Task_Sleep( 10 ); Task_LogString( " ", 1 ); }
Task_LogNewLine();

if (0) {
Task_Sleep( 1 );
char const cmd[] = "Desktop";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
asm ( "bkpt 6" );
}

if (0) {
  {
    static uint32_t const all_messages[] = { 0 };
    register uint32_t ver asm( "r0" ) = 300;
    register uint32_t task asm( "r1" ) = 0x4B534154;
    register char const *desc asm( "r2" ) = "CKernel boot";
    register uint32_t const *msgs asm( "r3" ) = all_messages;
    register uint32_t handle asm( "r1" );
    asm ( "svc %[swi]" 
      : "=r" (ver)
      , "=r" (handle)
      : [swi] "i" (XWimp_Initialise)
      , "r" (ver)
      , "r" (task)
      , "r" (desc)
      , "r" (msgs) );
    if (ver < 300) asm ( "bkpt 1" );
  }

  uint32_t window = 0;

  typedef union {
    char string[12];
    struct {
      void *p1;
      void *p2;
      uint32_t len;
    } indirect;
  } icon_data;

  typedef struct __attribute__(( packed )) {
    uint32_t visible_area_x_min;
    uint32_t visible_area_y_min;
    uint32_t visible_area_x_max;
    uint32_t visible_area_y_max;

    int32_t  scroll_offset_x;
    int32_t  scroll_offset_y;
    uint32_t behind;
    uint32_t flags;

    uint8_t   title_fg;
    uint8_t   title_bg;
    uint8_t   work_fg;
    uint8_t   work_bg;

    uint8_t   scroll_outer;
    uint8_t   scroll_slider;
    uint8_t   focus_title_bg;
    uint8_t   extra_flags;

    uint32_t work_area_x_min;
    uint32_t work_area_y_min;
    uint32_t work_area_x_max;
    uint32_t work_area_y_max;

    uint32_t title_icon_flags;
    uint32_t window_button_types;
    uint32_t *sprite_acb;

    uint16_t min_w;
    uint16_t min_h;

    icon_data title_data;

    uint32_t number_of_icons;

    struct {
      uint32_t minx;
      uint32_t miny;
      uint32_t maxx;
      uint32_t maxy;
      uint32_t flags;
      icon_data data;
    } icon_block[];
  } window_block;

  if (offset_of( window_block, number_of_icons ) != 84) for (;;) asm ( "bkpt 1 " );

  {
    window_block win;

    // Needed? Set by OpenWindow, no?
    win.visible_area_x_min = 200;
    win.visible_area_y_min = 200;
    win.visible_area_x_max = 1200;
    win.visible_area_y_max = 1600;

    win.scroll_offset_x = 250;
    win.scroll_offset_y = -400;
    win.behind = -1;

    //             3     2   2   1     1       
    //             0     4   0   6     0 8       0
    win.flags = 0b11111111001000010000000000000010;

    // 0 White
    // 7 Black
    // 8 Brown?
    // 9 Cyan
    // 10 Green
    // 11 Blue
    // 12 Light blue
    // 13 Dull green?
    win.title_fg = 7;
    win.title_bg = 9;
    win.work_fg = 7;
    win.work_bg = 14;

    win.scroll_outer = 5;
    win.scroll_slider = 12;
    win.focus_title_bg = 15;
    win.extra_flags = 0;

    win.work_area_x_min = 0;
    win.work_area_y_min = 0;
    win.work_area_x_max = 10000;
    win.work_area_y_max = 15000;

    win.title_icon_flags = 0x101;

    win.window_button_types = 0;
    win.sprite_acb = (void*) 1;

    win.min_w = 300;
    win.min_h = 300;

    win.title_data.indirect.p1 = "Hello";
    win.title_data.indirect.p2 = 0;
    win.title_data.indirect.len = 5;

    win.number_of_icons = 0;

    register void *w asm( "r1" ) = &win;
    register uint32_t handle asm( "r0" );
    asm ( "svc %[swi]" 
      : "=r" (handle)
      : [swi] "i" (XWimp_CreateWindow)
      , "r" (w)
      : "memory" );
    if (handle == 0) asm ( "bkpt 1" );
    window = handle;
  }

  if (1) {
    struct {
      uint32_t handle;

      uint32_t visible_area_x_min;
      uint32_t visible_area_y_min;
      uint32_t visible_area_x_max;
      uint32_t visible_area_y_max;

      uint32_t scroll_offset_x;
      uint32_t scroll_offset_y;
      uint32_t behind;
    } open = { window, 400, 200, 700, 800, 0, 0, -1 };

          uint32_t const *block = &open.handle;
          Task_LogSmallNumber( block[1] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[2] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[3] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[4] ); Task_LogNewLine();
    register void *o asm( "r1" ) = &open;
    asm ( "svc %[swi]" 
      :
      : [swi] "i" (XWimp_OpenWindow)
      , "r" (o)
      : "memory", "r0" );
  }

  uint32_t block[64];
  bool started_desktop = 0;

  for (;;) {
    register uint32_t f asm( "r0" ) = 0b11100011100000110000 | started_desktop;
    register uint32_t *b asm( "r1" ) = block;
    register uint32_t d asm( "r2" ) = 10;
    register uint32_t e asm( "r0" );
    register uint32_t *rb asm( "r1" );
    asm ( "svc %[swi]" 
      : "=r" (e)
      , "=r" (rb)
      : [swi] "i" (XWimp_PollIdle)
      , "r" (f)
      , "r" (b)
      , "r" (d) );
    uint32_t event = e;
    uint32_t *retb = rb;
    Task_LogSmallNumber( event );
    Task_LogNewLine();

    if (retb != block) asm ( "bkpt 1" );

    switch (event) {
    case Null_Reason_Code: { char const text[] = "Null_Reason_Code\n"; Task_LogString( text, sizeof( text )-1 ); }
      if (1) {
        char const cmd[] = "Desktop"; // Resources:$.Apps.!Edit.!Run";
        register char const *s asm( "r0" ) = cmd;
        asm ( "svc %[swi]" : : [swi] "i" (XWimp_StartTask), "r" (s) );
        started_desktop = 1;
      }
        started_desktop = 1;
      break;
    case Redraw_Window_Request:
      { char const text[] = "Redraw_Window_Request\n"; Task_LogString( text, sizeof( text )-1 ); }
      {
        register uint32_t m asm( "r0" );
        register void *b asm( "r1" ) = block;
        asm ( "svc %[swi]" 
          : "=r" (m)
          : [swi] "i" (XWimp_RedrawWindow)
          , "r" (b) );
        if (m) do {
          Task_LogSmallNumber( block[1] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[2] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[3] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[4] ); Task_LogNewLine();

          Task_LogSmallNumber( block[5] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[6] ); Task_LogNewLine();

          Task_LogSmallNumber( block[7] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[8] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[9] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( block[10] ); Task_LogNewLine();

          register uint32_t m asm( "r0" );
          register void *b asm( "r1" ) = block;
          asm ( "svc %[swi]" 
            : "=r" (m)
            : [swi] "i" (XWimp_GetRectangle)
            , "r" (b) );
        } while (m);
      { char const text[] = "Redraw complete\n"; Task_LogString( text, sizeof( text )-1 ); }
      }

      break;
    case Open_Window_Request:
      { char const text[] = "Open_Window_Request\n"; Task_LogString( text, sizeof( text )-1 ); }
      {
          Task_LogHex( retb[0] ); Task_LogString( ": ", 2 );
          Task_LogSmallNumber( retb[1] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( retb[2] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( retb[3] ); Task_LogString( ", ", 2 );
          Task_LogSmallNumber( retb[4] ); Task_LogNewLine();
        register void *o asm( "r1" ) = retb;
        asm ( "svc %[swi]" 
          :
          : [swi] "i" (XWimp_OpenWindow)
          , "r" (o)
          : "memory", "r0" );
      }
      break;
    case Close_Window_Request: { char const text[] = "Close_Window_Request\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Pointer_Leaving_Window: { char const text[] = "Pointer_Leaving_Window\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Pointer_Entering_Window: { char const text[] = "Pointer_Entering_Window\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Mouse_Click: { char const text[] = "Mouse_Click\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case User_Drag_Box: { char const text[] = "User_Drag_Box\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Key_Pressed: { char const text[] = "Key_Pressed\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Menu_Selection: { char const text[] = "Menu_Selection\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Scroll_Request: { char const text[] = "Scroll_Request\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Lose_Caret: { char const text[] = "Lose_Caret\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Gain_Caret: { char const text[] = "Gain_Caret\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case Poll_word_non_zero: { char const text[] = "Poll_word_non_zero\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case User_Message: { char const text[] = "User_Message\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case User_Message_Recorded: { char const text[] = "User_Message_Recorded\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    case User_Message_Acknowledge: { char const text[] = "User_Message_Acknowledge\n"; Task_LogString( text, sizeof( text )-1 ); break; }
    default:
      asm ( "bkpt 1" );
    }
  }
}

  if (1) {
    char const text[] ="Setting mode X1920 Y1080 C16M F60 EX0 EY0\n";
    Task_LogString( text, sizeof( text ) - 1 );

    register char const *mode asm( "r1" ) = &text[13];
    register uint32_t code asm( "r0" ) = 15; // "Select screen mode by string"
    asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ScreenMode)
      , "r" (code)
      , "r" (mode) );
  }

{
if (0) {
  // Logical screen is 3840x2160!
  // Nice big triangle, works.
  Plot( 4, 100, 100 );
  Plot( 4, 1820, 980 );
  Plot( 85, 1820, 100 );

  // Fill whole screen
  Plot( 4, 0, 0 );
  Plot( 101, 3839, 2159 );
}

if (0) {
  uint32_t found_font = 0;

  enum { Font_FindFont = 0x40081, Font_Paint = 0x40086 };
  {
    // Note: 12 (14?) point or smaller requires SuperSample module
    register uint32_t handle asm( "r0" );
    register char const *font asm( "r1" ) = "Homerton.Bold";
    register uint32_t xpoint16 asm( "r2" ) = 16 * 24;
    register uint32_t ypoint16 asm( "r3" ) = 16 * 24;
    register uint32_t xdpi asm( "r4" ) = 0;
    register uint32_t ydpi asm( "r5" ) = 0;
    asm ( "svc %[swi]" 
      : "=r" (handle)
      , "=r" (xdpi)
      , "=r" (ydpi)
      : [swi] "i" (Font_FindFont)
      , "r" (font)
      , "r" (xpoint16)
      , "r" (ypoint16)
      , "r" (xdpi)
      , "r" (ydpi)
      );
    found_font = handle; // Preserve for posterity, or at least this section
  }

  {
    enum { ColourTrans_SetFontColours = 0x4074f };

    register uint32_t handle asm( "r0" ) = found_font;
    register uint32_t bg asm( "r1" ) = 0;
    register uint32_t fg asm( "r2" ) = 0xffff0000;      // Yellow
    register uint32_t offset asm( "r3" ) = 14;
    asm ( "svc %[swi]" 
      :
      : [swi] "i" (ColourTrans_SetFontColours)
      , "r" (handle)
      , "r" (fg)
      , "r" (bg)
      , "r" (offset)
      );
  }

  {
    char const text[] = "RISC OS";
    Task_LogString( "Painting ", 9 );
    Task_LogString( text, sizeof( text ) - 1 );
    Task_LogNewLine();

    register uint32_t handle asm( "r0" ) = found_font;
    register char const *s asm( "r1" ) = text;
    //register uint32_t flags asm( "r2" ) = 0x190; // With length in R7
    register uint32_t flags asm( "r2" ) = 0x110;
    register uint32_t xpos asm( "r3" ) = 20;
    register uint32_t ypos asm( "r4" ) = 800;
    register uint32_t len asm( "r7" ) = sizeof( text ) - 1;
    asm ( "svc %[swi]" 
      :
      : [swi] "i" (Font_Paint)
      , "r" (handle)
      , "r" (flags)
      , "r" (s)
      , "r" (xpos)
      , "r" (ypos)
      , "r" (len)
      );
    Task_LogString( "Painted\n", 8 );
  }

  {
    char const cmd[] = "FontList\n";
    register char const *s asm( "r0" ) = cmd;
    asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
  }
}
}

if (0) {
  //Task_Sleep( 1 );
  // Tell modified qemu to log CPU if logging int

  asm volatile ( "svc %[swi]" : : [swi] "i" (0x66666) : "r0");
}

{
char const cmd[] = "Desktop Resources:$.Apps.!Alarm";
register char const *s asm( "r0" ) = cmd;
asm ( "svc %[swi]" : : [swi] "i" (OS_CLI), "r" (s) );
}
    Task_LogString( "Desktop returned!\n", 17 );

Task_Sleep( 1 );
asm ( "bkpt 2" );
  register uint32_t enter asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules."DEFAULT_LANGUAGE;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
    "\n  movvc r0, #0"
    : "=r" (error)
    : [swi] "i" (OS_Module), "r" (enter), "r" (module)
    : "cc", "memory" );

  for (;;) { asm ( "udf 8" ); Task_Sleep( 1000000 ); }

  __builtin_unreachable();
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

