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

// This file contains functions that are useful to users of OSTask

#include "CK_types.h"

enum {
    OSTask_Yield = 0x2c0        // Like Sleep(0), but preserves all registers
  , OSTask_Sleep                // Milliseconds, counting from the next one
  , OSTask_Create               // 0x2c2 New OSTask in the same slot
  , OSTask_Spawn                // 0x2c3 New OSTask in a new slot
  , OSTask_EndTask              // 0x2c4 Last one out ends the slot
  , OSTask_Cores                // 0x2c5 Use sparingly! (More or less
                                // useless outside interrupt and memory
                                // management tasks, I think).
  , OSTask_RegisterSWIHandlers  // 0x2c6 Code or queue for each SWI
  , OSTask_MapDevicePages       // 0x2c7 For device drivers
  , OSTask_AppMemoryTop         // 0x2c8 r0 = New value, or 0 to read

  // To service (and, with permission, take over) other tasks
  , OSTask_RunForTask           // 0x2c9 Run code in the context of the task
  , OSTask_GetRegisters         // 0x2ca Get registers of controlled task
  , OSTask_SetRegisters         // 0x2cb Set registers of controlled task
  , OSTask_Finished             // 0x2cc Return to home context
  , OSTask_ReleaseTask          // 0x2cd Resume and release control of task
  , OSTask_ChangeController     // 0x2ce Pass the controlled task to another
  , OSTask_SetController        // 0x2cf Allow another task to control me

  // Resource protection
  , OSTask_LockClaim            // 0x2d0
  , OSTask_LockRelease          // 0x2d1

  // Interrupt handling (for use by the module dealing with the interrupt
  // controller)
  , OSTask_EnablingInterrupts   // 0x2d2 Disable IRQs while I get
  , OSTask_WaitForInterrupt     // 0x2d3 ready to wait.

  , OSTask_PhysicalFromVirtual  // 0x2d4 For device drivers
                                // (Also flushes the cache)
  , OSTask_InvalidateCache      // 0x2d5 Area of RAM may have been changed
  , OSTask_FlushCache           // 0x2d6 Ensure changes are visible to
                                // external hardware.

  , OSTask_SwitchToCore         // 0x2d7 Use sparingly!

  // SWI for internal use only!
  , OSTask_Tick                 // 0x2d8 For one (interrupt) task to use.
                                // Call every ms.

  , OSTask_MapFrameBuffer // Depricated, I think... TBC

  , OSTask_GetLogPipe           // 0x2da For the current core, to read.
  , OSTask_LogString            // 0x2db to the log pipe (any task).

  , OSTask_PipeCreate = OSTask_Yield + 32 // 0x2e0
  , OSTask_PipeWaitForSpace
  , OSTask_PipeSpaceFilled
  , OSTask_PipeSetSender
  , OSTask_PipeUnreadData
  , OSTask_PipeNoMoreData
  , OSTask_PipeWaitForData
  , OSTask_PipeDataConsumed
  , OSTask_PipeSetReceiver
  , OSTask_PipeNotListening
  , OSTask_PipeWaitUntilEmpty

  , OSTask_QueueCreate = OSTask_PipeCreate + 16 // 0x2f0
  , OSTask_QueueDelete
  , OSTask_QueueWait
  , OSTask_QueueWaitCore        // No implementation
  , OSTask_QueueWaitSWI         // No implementation
  , OSTask_QueueWaitCoreAndSWI  // No implementation

  , OSTask_QueueR12             // For modules to route SWIs to providers
};

// "memory" clobber, because the task might have moved cores by
// the time this returns.
static inline
void Task_Sleep( uint32_t ms )
{
#ifdef XQEMU
  ms = ms / 10;
#endif
  register uint32_t t asm( "r0" ) = ms;
  // sets r0 to 0. volatile is needed, since the optimiser ignores
  // asm statements with an output which is ignored.
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    : "=r" (t)
    : [swi] "i" (OSTask_Sleep), "r" (t)
    : "lr", "cc", "memory" );
}

static inline
void Task_Yield()
{
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]" 
      :
      : [swi] "i" (OSTask_Yield)
      : "lr", "cc", "memory" );
}

static inline
void Task_EndTask()
{
  asm ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_EndTask)
    : "lr", "cc" );
}

typedef union swi_action swi_action;
union swi_action {
  void (*code)( svc_registers *regs, void *ws, int core, uint32_t task );
  uint32_t queue;
};

typedef struct swi_handlers {
  swi_action action[64];
} swi_handlers;

typedef union core_info core_info;
union core_info {
  struct {
    uint32_t current:16;
    uint32_t total:16;
  };
  uint32_t raw;
};

// The returned core number will remain the same until the task
// calls any variation on Sleep, ClaimLock, etc. The interrupt task
// SWIs will not change the core.
static inline
core_info Task_Cores()
{
  register uint32_t raw asm ( "r0" );
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    : "=r" (raw)
    : [swi] "i" (OSTask_Cores)
    : "lr", "cc" );
  core_info cores = { .raw = raw };
  return cores;
}

static inline
void Task_RegisterSWIHandlers( swi_handlers const *h )
{
  register swi_handlers const *handlers asm ( "r0" ) = h;

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_RegisterSWIHandlers)
    , "r" (handlers)
    : "lr", "cc", "memory" );
}

// Note: base page is physical address >> 12
static inline
void *Task_MapDevicePages( void volatile *va, uint32_t base_page, uint32_t pages )
{
  register void volatile *virt asm ( "r0" ) = va;
  register uint32_t page asm ( "r1" ) = base_page;
  register uint32_t number asm ( "r2" ) = pages;

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_MapDevicePages)
    , "r" (virt)
    , "r" (page)
    , "r" (number)
    : "lr", "cc", "memory" );

  return (void*) va;
}

static inline
uint32_t Task_SetAppMemoryTop( uint32_t new_top )
{
  register uint32_t top asm ( "r0" ) = new_top;

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    : "=r" (top)
    : [swi] "i" (OSTask_AppMemoryTop)
    , "r" (top)
    : "lr", "cc", "memory" );

  return top;
}

static inline
uint32_t Task_ReadAppTop()
{
  return Task_SetAppMemoryTop( 0 );
}

static inline
void Task_EnablingInterrupts()
{
  asm ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_EnablingInterrupts)
    : "lr", "cc", "memory" );
}

static inline
void Task_WaitForInterrupt()
{
  asm ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_WaitForInterrupt)
    : "lr", "cc", "memory" );
}

static inline
void Task_SwitchToCore( uint32_t core )
{
  register uint32_t c asm ( "r0" ) = core;

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_SwitchToCore)
    , "r" (c)
    : "lr", "cc", "memory" );
}

static inline
void *Task_MapFrameBuffer( uint32_t pa, uint32_t pages )
{
  register uint32_t physical asm ( "r0" ) = pa;
  register uint32_t size asm ( "r1" ) = pages;

  register void *va asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvs r0, #0"
        : "=r" (va)
        : [swi] "i" (OSTask_MapFrameBuffer)
        , "r" (physical)
        , "r" (size)
        : "lr", "cc", "memory" );

  return va;
}

static inline
uint32_t Task_GetLogPipe()
{
  register uint32_t pipe asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    : "=r" (pipe)
    : [swi] "i" (OSTask_GetLogPipe)
    : "lr", "cc", "memory" );

  return pipe;
}

static inline
void Task_LogString( char const *string, uint32_t length )
{
#ifdef DEBUG__QUIET
  return;
#endif
  if (length == 0) {
    char const *p = string;
    while (*p != '\0') {
      p ++;
    }
    length = p - string;
  }

  register char const *s asm ( "r0" ) = string;
  register uint32_t l asm ( "r1" ) = length;

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
    :
    : [swi] "i" (OSTask_LogString)
    , "r" (s)
    , "r" (l)
    : "lr", "cc", "memory" );
}

static inline __attribute__(( optimize( "Os" ) ))
void Task_LogSmallNumber( uint32_t number )
{
#ifdef DEBUG__QUIET
  return;
#endif
  // FIXME: This is very chunky, maybe use OS_Convert...
  // putting this implmentation into NoLegacy.
  char string[12];
  int i = 0;
  if (number >= 10000000000) {
    char d = '0';
    while (number >= 10000000000) {
      d++; number -= 10000000000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 1000000000) {
    char d = '0';
    while (number >= 1000000000) {
      d++; number -= 1000000000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 100000000) {
    char d = '0';
    while (number >= 100000000) {
      d++; number -= 100000000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 10000000) {
    char d = '0';
    while (number >= 10000000) {
      d++; number -= 10000000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 1000000) {
    char d = '0';
    while (number >= 1000000) {
      d++; number -= 1000000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 100000) {
    char d = '0';
    while (number >= 100000) {
      d++; number -= 100000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 10000) {
    char d = '0';
    while (number >= 10000) {
      d++; number -= 10000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 1000) {
    char d = '0';
    while (number >= 1000) {
      d++; number -= 1000;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 100) {
    char d = '0';
    while (number >= 100) {
      d++; number -= 100;
    }
    string[i++] = d;
  }

  if (i > 0 || number >= 10) {
    char d = '0';
    while (number >= 10) {
      d++; number -= 10;
    }
    string[i++] = d;
  }

  if (number >= 0) {
    string[i++] = '0' + number;
  }

  Task_LogString( string, i );
}

static inline char tohex( uint8_t n )
{
  return ("0123456789abcdef")[n];
}

static __attribute__(( noinline )) // inline
void Task_LogHex( uint32_t number )
{
#ifdef DEBUG__QUIET
  return;
#endif
  char string[8];
  for (int i = 0; i < 8; i++) {
    string[7-i] = tohex( (number >> (i * 4)) & 0xf );
  }

  Task_LogString( string, 8 );
}

static inline
void Task_LogHexP( void const *pointer )
{
  Task_LogHex( (uint32_t) pointer );
}

static inline
void Task_LogNewLine()
{
  Task_LogString( "\n", 1 );
}

static inline
void Task_Space()
{
  Task_LogString( " ", 1 );
}

typedef struct {
  error_block *error;
  void *location;
  uint32_t available;
} PipeSpace;

static inline
uint32_t PipeOp_CreateForTransfer( uint32_t max_block )
{
  register uint32_t max_block_size asm ( "r1" ) = max_block;
  register uint32_t max_data asm ( "r2" ) = 0; // Unlimited
  register uint32_t allocated_mem asm ( "r3" ) = 0; // OS allocated

  register uint32_t pipe asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc", "memory" );

  return pipe;
}

static inline
uint32_t PipeOp_Create( void *base, uint32_t max_block )
{
  register uint32_t max_block_size asm ( "r1" ) = max_block;
  register uint32_t max_data asm ( "r2" ) = 0; // Unlimited
  register void *allocated_mem asm ( "r3" ) = base;

  register uint32_t pipe asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc", "memory" );

  return pipe;
}

static inline
uint32_t PipeOp_CreateOnBuffer( void *buffer, uint32_t len )
{
  // Fixed length pipe
  register uint32_t max_block_size asm ( "r1" ) = len;
  register uint32_t max_data asm ( "r2" ) = len;
  register void *allocated_mem asm ( "r3" ) = buffer;

  register uint32_t pipe asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc", "memory" );

  return pipe;
}

// This routine will return immediately if the requested space exceeds the
// capacity.
// This routine will return early if NotListening (-> space.location = 0) is
// (or has been) called.
// Data consumers, if they want to consume fixed-sized blocks at a time,
// should allocate at least one extra block of capacity.
static inline
PipeSpace PipeOp_WaitForSpace( uint32_t write_pipe, uint32_t bytes )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = write_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeWaitForSpace)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory" );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline
PipeSpace PipeOp_SpaceFilled( uint32_t write_pipe, uint32_t bytes )
{
  // IN
  // In this case, bytes represents the number of bytes that the caller has written and
  // is making available to the reader.
  // The returned information is the same as from WaitForSpace and indicates the remaining
  // space after the filled bytes have been accepted. The virtual address of the remaining
  // data may not be the same as the address of the byte after the last accepted byte.
  register uint32_t pipe asm ( "r0" ) = write_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeSpaceFilled)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory" );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline
PipeSpace PipeOp_WaitForData( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = read_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeWaitForData)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory" );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline
PipeSpace PipeOp_DataConsumed( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  // In this case, the bytes are the number of bytes no longer of interest.
  // The returned information is the same as from WaitForData and indicates
  // the remaining data after the consumed bytes have been removed. The
  // virtual address of the remaining data may not be the same as the
  // address of the byte after the last consumed byte.
  register uint32_t pipe asm ( "r0" ) = read_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeDataConsumed)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory" );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline
error_block *PipeOp_SetReceiver( uint32_t read_pipe, uint32_t new_receiver )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = read_pipe;
  register uint32_t task asm ( "r1" ) = new_receiver;

  // OUT
  register error_block *error asm ( "r0" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeSetReceiver)
        , "r" (pipe)
        , "r" (task)
        : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *PipeOp_SetSender( uint32_t write_pipe, uint32_t new_sender )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = write_pipe;
  register uint32_t task asm ( "r1" ) = new_sender;

  // OUT
  register error_block *error;

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc %[error], #0"
    "\n  movvs %[error], r0"

        : [error] "=r" (error)
        : [swi] "i" (OSTask_PipeSetSender)
        , "r" (pipe)
        , "r" (task)
        : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *PipeOp_NotListening( uint32_t read_pipe )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = read_pipe;

  // OUT
  register error_block *error asm ( "r0" );

  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeNotListening)
        , "r" (pipe)
        : "lr", "cc", "memory" );

  return error;
}


static inline
error_block *PipeOp_NoMoreData( uint32_t send_pipe )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = send_pipe;

  // OUT
  register error_block *error asm ( "r0" );

  asm volatile (
        "subs r0, r0, #0\n  svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeNoMoreData)
        , "r" (pipe)
        : "lr", "cc" );

  return error;
}

static inline
error_block *Task_RunForTask( uint32_t client )
{
  register uint32_t h asm ( "r0" ) = client;
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_RunForTask)
      , "r" (h)
      : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *Task_Finished()
{
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_Finished)
      : "lr", "cc", "memory" );

  return error;
}

// Task creation routines never return an error, but may never return (until
// resources become available, which may be a user decision).

// The number is the number of parameters passed to the task (in addition to
// the task handle), the remainder will be zeros.

// These routines allow the created task to run immediately, the usual case:
// Task_SpawnTask{3,2,1,0}
// Task_CreateTask{3,2,1,0}

// These routines leave the created task blocked, and under the control of
// the caller.
// The caller can then pass it off to another task, ask them to run code
// on its behalf (and with its permissions), or release it to run freely.
// Task_SpawnService{4,3,2,1,0}
// Task_CreateService{4,3,2,1,0}
// See also Queues, which put tasks under the control of the task that
// removes them from a queue.

static inline
uint32_t Task_SpawnTask4( void *start, uint32_t sp, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3 )
{
  register void *s asm( "r0" ) = start;
  register uint32_t p asm( "r1" ) = sp;
  register uint32_t r1 asm( "r2" ) = p0;
  register uint32_t r2 asm( "r3" ) = p1;
  register uint32_t r3 asm( "r4" ) = p2;
  register uint32_t r4 asm( "r5" ) = p3;
  register uint32_t handle asm( "r0" );
  asm volatile ( // Volatile because we ignore the outputs
        "svc %[swi_spawn]"
    "\n  mov r1, #0"    // No extra context
    "\n  svc %[swi_release]"
    : "=r" (p) // Corrupted
    , "=r" (handle)
    : [swi_spawn] "i" (OSTask_Spawn)
    , [swi_release] "i" (OSTask_ReleaseTask)
    , "r" (s)
    , "r" (p)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    , "r" (r4)
    : "lr", "cc" );

  return handle;
}

static inline
uint32_t Task_SpawnTask3( void *start, uint32_t sp, uint32_t p0, uint32_t p1, uint32_t p2 )
{
  return Task_SpawnTask4( start, sp, p0, p1, p2, 0 );
}

static inline
uint32_t Task_SpawnTask2( void *start, uint32_t sp, uint32_t p0, uint32_t p1 )
{
  return Task_SpawnTask3( start, sp, p0, p1, 0 );
}

static inline
uint32_t Task_SpawnTask1( void *start, uint32_t sp, uint32_t p0 )
{
  return Task_SpawnTask2( start, sp, p0, 0 );
}

static inline
uint32_t Task_SpawnTask0( void *start, uint32_t sp )
{
  return Task_SpawnTask1( start, sp, 0 );
}

static inline
uint32_t Task_CreateTask4( void *start, uint32_t sp, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3 )
{
  register void *s asm( "r0" ) = start;
  register uint32_t p asm( "r1" ) = sp;
  register uint32_t r1 asm( "r2" ) = p0;
  register uint32_t r2 asm( "r3" ) = p1;
  register uint32_t r3 asm( "r4" ) = p2;
  register uint32_t r4 asm( "r5" ) = p3;
  register uint32_t handle asm( "r0" );
  asm volatile ( // Volatile because we ignore the outputs
        "svc %[swi_create]"
    "\n  mov r1, #0"    // No extra context
    "\n  svc %[swi_release]"
    : "=r" (p) // Corrupted
    , "=r" (handle)
    : [swi_create] "i" (OSTask_Create)
    , [swi_release] "i" (OSTask_ReleaseTask)
    , "r" (s)
    , "r" (p)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    , "r" (r4)
    : "lr", "cc" );

  return handle;
}

static inline
uint32_t Task_CreateTask3( void *start, uint32_t sp, uint32_t p0, uint32_t p1, uint32_t p2 )
{
  return Task_CreateTask4( start, sp, p0, p1, p2, 0 );
}

static inline
uint32_t Task_CreateTask2( void *start, uint32_t sp, uint32_t p0, uint32_t p1 )
{
  return Task_CreateTask3( start, sp, p0, p1, 0 );
}

static inline
uint32_t Task_CreateTask1( void *start, uint32_t sp, uint32_t p0 )
{
  return Task_CreateTask2( start, sp, p0, 0 );
}

static inline
uint32_t Task_CreateTask0( void *start, uint32_t sp )
{
  return Task_CreateTask1( start, sp, 0 );
}

static inline
uint32_t Task_SpawnService3( void *start, uint32_t sp, uint32_t p0, uint32_t p1, uint32_t p2 )
{
  register void *s asm( "r0" ) = start;
  register uint32_t p asm( "r1" ) = sp;
  register uint32_t r1 asm( "r2" ) = p0;
  register uint32_t r2 asm( "r3" ) = p1;
  register uint32_t r3 asm( "r4" ) = p2;
  register uint32_t handle asm( "r0" );
  asm volatile ( // Volatile because we ignore the outputs
        "svc %[swi_spawn]"
    : "=r" (handle)
    : [swi_spawn] "i" (OSTask_Spawn)
    , "r" (s)
    , "r" (p)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    : "lr", "cc" );

  return handle;
}

static inline
uint32_t Task_SpawnService2( void *start, uint32_t sp, uint32_t p0, uint32_t p1 )
{
  return Task_SpawnService3( start, sp, p0, p1, 0 );
}

static inline
uint32_t Task_SpawnService1( void *start, uint32_t sp, uint32_t p0 )
{
  return Task_SpawnService2( start, sp, p0, 0 );
}

static inline
uint32_t Task_SpawnService0( void *start, uint32_t sp )
{
  return Task_SpawnService1( start, sp, 0 );
}

static inline
uint32_t Task_CreateService3( void *start, uint32_t sp, uint32_t p0, uint32_t p1, uint32_t p2 )
{
  register void *s asm( "r0" ) = start;
  register uint32_t p asm( "r1" ) = sp;
  register uint32_t r1 asm( "r2" ) = p0;
  register uint32_t r2 asm( "r3" ) = p1;
  register uint32_t r3 asm( "r4" ) = p2;
  register uint32_t handle asm( "r0" );
  asm volatile ( // Volatile because we ignore the outputs
        "svc %[swi_create]"
    : "=r" (handle)
    : [swi_create] "i" (OSTask_Create)
    , "r" (s)
    , "r" (p)
    , "r" (r1)
    , "r" (r2)
    , "r" (r3)
    : "lr", "cc" );

  return handle;
}

static inline
uint32_t Task_CreateService2( void *start, uint32_t sp, uint32_t p0, uint32_t p1 )
{
  return Task_CreateService3( start, sp, p0, p1, 0 );
}

static inline
uint32_t Task_CreateService1( void *start, uint32_t sp, uint32_t p0 )
{
  return Task_CreateService2( start, sp, p0, 0 );
}

static inline
uint32_t Task_CreateService0( void *start, uint32_t sp )
{
  return Task_CreateService1( start, sp, 0 );
}

static inline
error_block *Task_ReleaseTask( uint32_t client, svc_registers const *regs )
{
  register uint32_t h asm ( "r0" ) = client;
  register svc_registers const *r asm ( "r1" ) = regs;
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_ReleaseTask)
      , "r" (h)
      , "r" (r)
      : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *Task_ChangeController( uint32_t client, uint32_t controller )
{
  register uint32_t h asm ( "r0" ) = client;
  register uint32_t replacement asm ( "r1" ) = controller;
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_ChangeController)
      , "r" (h)
      , "r" (replacement)
      : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *Task_SetController( svc_registers *regs, uint32_t controller )
{
  register svc_registers *r asm ( "r0" ) = regs;
  register uint32_t c asm ( "r1" ) = controller;
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_SetController)
      , "r" (r)
      , "r" (c)
      : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *Task_GetRegisters( uint32_t controlled, svc_registers *regs )
{
  register uint32_t h asm ( "r0" ) = controlled;
  register svc_registers *r asm ( "r1" ) = regs;
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_GetRegisters)
      , "r" (h)
      , "r" (r)
      : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *Task_SetRegisters( uint32_t controlled, svc_registers *regs )
{
  register uint32_t h asm ( "r0" ) = controlled;
  register svc_registers *r asm ( "r1" ) = regs;
  register error_block *error asm ( "r0" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_SetRegisters)
      , "r" (h)
      , "r" (r)
      : "lr", "cc", "memory" );

  return error;
}

typedef struct {
  uint32_t task_handle;
  uint32_t swi;
  uint32_t core;
  error_block *error;
} queued_task;

static inline
uint32_t Task_QueueCreate()
{
  register uint32_t handle asm( "r0" );

  // FIXME handle errors
  // FIXME there should probably be a QueueDelete!
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             : "=r" (handle)
             : [swi] "i" (OSTask_QueueCreate)
             : "lr", "cc", "memory" );

  return handle;
}

static inline
queued_task Task_QueueWait( uint32_t queue_handle )
{
  queued_task result = { 0, 0 };

  register uint32_t handle asm ( "r0" ) = queue_handle;
  register uint32_t task_handle asm ( "r0" );
  register uint32_t swi asm ( "r1" );
  register uint32_t core asm ( "r2" );
  register error_block *error asm ( "r3" );

  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
             "\n  movvc r3, #0"
             "\n  movvs r3, r0"
             "\n  movvs r0, #0"
      : "=r" (task_handle)
      , "=r" (swi)
      , "=r" (core)
      , "=r" (error)
      : [swi] "i" (OSTask_QueueWait)
      , "r" (handle)
      : "lr", "cc", "memory" );

  result.task_handle = task_handle;
  result.swi = swi;
  result.core = core;
  result.error = error;

  return result;
}

// This function is for information only, to allow a task that
// owns a lock to wrap up what it's doing and release the lock
// if it detects another task waiting.
// Specifically, do NOT use it to decide not to call Release on
// the lock! (Another task on another core might call Claim before
// you know it.)
// DO NOT try to set the bit yourself! Use OSTask_LockClaim to
// do it for you (and put your task to sleep until released).
static inline
bool tasks_waiting_for( uint32_t *lock )
{
  return (1 & *lock) != 0;
}

// TODO: Make this only work with pipes, in fact, pass in
// pipe, offset, length.
static inline
uint32_t Task_PhysicalFromVirtual( void const *va, uint32_t length )
{
  register void const *v asm( "r0" ) = va;
  register uint32_t l asm( "r1" ) = length;
  register uint32_t p asm( "r0" );
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
      : "=r" (p)
      : [swi] "i" (OSTask_PhysicalFromVirtual)
      , "r" (v)
      , "r" (l)
      : "lr", "cc", "memory" );
  return p;
}

static inline
uint32_t Task_InvalidateCache( void const *va, uint32_t length )
{
  register void const *v asm( "r0" ) = va;
  register uint32_t l asm( "r1" ) = length;
  register uint32_t p asm( "r0" );
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
      : "=r" (p)
      : [swi] "i" (OSTask_InvalidateCache)
      , "r" (v)
      , "r" (l)
      : "lr", "cc", "memory" );
  return p;
}

static inline
uint32_t Task_FlushCache( void const *va, uint32_t length )
{
  register void const *v asm( "r0" ) = va;
  register uint32_t l asm( "r1" ) = length;
  register uint32_t p asm( "r0" );
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
      : "=r" (p)
      : [swi] "i" (OSTask_FlushCache)
      , "r" (v)
      , "r" (l)
      : "lr", "cc", "memory" );
  return p;
}

// These two can probably have the "lr" clobber removed; they're only
// called from usr32 mode.

// Returns true if lock has been reclaimed by the same task (in
// which case you don't want to release it this time).
// Pattern:
// void myfunc() {
//   bool reclaimed = Task_LockClaim( &mylock );
//   ...
//   if (!reclaimed) Task_LockRelease( &mylock );
// }
// If your code doesn't expect to re-claim the lock, a true result
// probably indicates a serious programming error.
static inline
bool Task_LockClaim( uint32_t *lock )
{
  register uint32_t *p asm( "r0" ) = lock;
  register bool reclaimed asm( "r0" );
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
      : "=r" (reclaimed)
      : [swi] "i" (OSTask_LockClaim)
      , "r" (p)
      : "lr", "cc", "memory" );
  return reclaimed;
}

// Release a lock, allowing any waiting tasks to claim it.
static inline
void Task_LockRelease( uint32_t *lock )
{
  register uint32_t *p asm( "r0" ) = lock;
  asm volatile ( "subs r0, r0, #0\n  svc %[swi]"
      :
      : [swi] "i" (OSTask_LockRelease)
      , "r" (p)
      : "lr", "cc", "memory" );
}

