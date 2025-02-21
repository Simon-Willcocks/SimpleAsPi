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

  register uint32_t enter asm ( "r0" ) = 0; // RMRun
  register char const *module asm ( "r1" ) = "System:Modules."DEFAULT_LANGUAGE;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
    "\n  movvc r0, #0"
    : "=r" (error)
    : [swi] "i" (OS_Module), "r" (enter), "r" (module)
    : "cc", "memory" );

  {
    char const text[] = "Default language returned!\n";
    Task_LogString( text, sizeof( text )-1 );
  }

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

