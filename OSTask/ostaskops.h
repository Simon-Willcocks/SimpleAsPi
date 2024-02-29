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
    OSTask_Yield = 0x300
  , OSTask_Sleep
  , OSTask_Create                       // New OSTask
  , OSTask_Spawn                        // New OSTask in a new slot
  , OSTask_EndTask                      // Last one out ends the slot
  , OSTask_Cores                        // Use sparingly! (More or less
                                        // useless outside interrupt tasks,
                                        // I think).
  , OSTask_RegisterSWIHandlers          // Code or queue
  , OSTask_MapDevicePages               // For device drivers
  , OSTask_AppMemoryTop                 // r0 = New value, or 0 to read
  , OSTask_RunThisForMe                 // Run code in the context of the task
  , OSTask_GetRegisters                 // Get registers of controlled task
  , OSTask_SetRegisters                 // Set registers of controlled task
  , OSTask_RelinquishControl            // Resume controlling task
  , OSTask_ReleaseTask                  // Resume controlled task
  , OSTask_ChangeController             // Pass the controlled task to another

  , OSTask_GetTaskHandle                // Current task - also passed to code
  , OSTask_LockClaim            // 0x310
  , OSTask_LockRelease

  , OSTask_EnablingInterrupts           // Disable IRQs while I get
  , OSTask_WaitForInterrupt             // ready to wait.

  , OSTask_PhysicalFromVirtual          // For device drivers
                                        // (Also flushes the cache)
  , OSTask_MemoryChanged                // Invalidates any cache

  , OSTask_SwitchToCore                 // Use sparingly!
  , OSTask_Tick                         // For HAL module use only

  , OSTask_MapFrameBuffer // Depricated, I think...

  , OSTask_GetLogPipe                   // For the current core
  , OSTask_LogString

  , OSTask_PipeCreate = OSTask_Yield + 32
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

  , OSTask_QueueCreate = OSTask_PipeCreate + 16
  , OSTask_QueueWait
  , OSTask_QueueWaitCore        // No implementation
  , OSTask_QueueWaitSWI         // No implementation
  , OSTask_QueueWaitCoreAndSWI  // No implementation
};

static inline
void Task_Sleep( uint32_t ms )
{
#ifdef XQEMU
  ms = ms / 10;
#endif
  register uint32_t t asm( "r0" ) = ms;
  // sets r0 to 0. volatile is needed, since the optimiser ignores
  // asm statements with an output which is ignored.
  asm volatile ( "svc %[swi]"
    : "=r" (t)
    : [swi] "i" (OSTask_Sleep), "r" (t)
    : "lr", "cc" );
}

static inline
void Task_Yield()
{
  asm volatile ( "svc %[swi]" : : [swi] "i" (OSTask_Yield) : "lr", "cc" );
}

static inline
void Task_EndTask()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_EndTask)
    : "lr", "cc" );
}

typedef union swi_action swi_action;
union swi_action {
  uint32_t code;
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
  asm volatile ( "svc %[swi]"
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

  asm volatile ( "svc %[swi]"
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

  asm volatile ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_MapDevicePages)
    , "r" (virt)
    , "r" (page)
    , "r" (number)
    : "lr", "cc" );

  return (void*) va;
}

static inline
void Task_EnablingInterrupts()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_EnablingInterrupts)
    : "lr", "cc" );
}

static inline
void Task_WaitForInterrupt()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_WaitForInterrupt)
    : "lr", "cc" );
}

static inline
void Task_SwitchToCore( uint32_t core )
{
  register uint32_t c asm ( "r0" ) = core;

  asm volatile ( "svc %[swi]"
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

  asm volatile ( "svc %[swi]"
             "\n  movvs r0, #0"
        : "=r" (va)
        : [swi] "i" (OSTask_MapFrameBuffer)
        , "r" (physical)
        , "r" (size)
        : "lr", "cc" );

  return va;
}

static inline
uint32_t Task_GetLogPipe()
{
  register uint32_t pipe asm ( "r0" );

  asm volatile ( "svc %[swi]"
    : "=r" (pipe)
    : [swi] "i" (OSTask_GetLogPipe)
    : "lr", "cc" );

  return pipe;
}

static inline
void Task_LogString( char const *string, uint32_t length )
{
  if (length == 0) {
    char const *p = string;
    while (*p != '\0') {
      p ++;
    }
    length = p - string;
  }

  register char const *s asm ( "r0" ) = string;
  register uint32_t l asm ( "r1" ) = length;

  asm volatile ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_LogString)
    , "r" (s)
    , "r" (l)
    : "lr", "cc", "memory" );
}

static inline __attribute__(( optimize( "Os" ) ))
void Task_LogSmallNumber( uint32_t number )
{
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

static inline
void Task_LogHex( uint32_t number )
{
  char string[8];
  for (int i = 0; i < 8; i++) {
    string[7-i] = tohex( (number >> (i * 4)) & 0xf );
  }

  Task_LogString( string, 8 );
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

  asm volatile ( "svc %[swi]"
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc" );

  return pipe;
}

static inline
uint32_t PipeOp_Create( void *base, uint32_t max_block )
{
  register uint32_t max_block_size asm ( "r1" ) = max_block;
  register uint32_t max_data asm ( "r2" ) = 0; // Unlimited
  register void *allocated_mem asm ( "r3" ) = base;

  register uint32_t pipe asm ( "r0" );

  asm volatile ( "svc %[swi]"
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc" );

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

  asm volatile ( "svc %[swi]"
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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeWaitForSpace)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeSpaceFilled)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeWaitForData)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeDataConsumed)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeSetReceiver)
        , "r" (pipe)
        , "r" (task)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc %[error], #0"
    "\n  movvs %[error], r0"

        : [error] "=r" (error)
        : [swi] "i" (OSTask_PipeSetSender)
        , "r" (pipe)
        , "r" (task)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeNotListening)
        , "r" (pipe)
        : "lr", "cc" );

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
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeNoMoreData)
        , "r" (pipe)
        : "lr", "cc" );

  return error;
}

static inline
error_block *Task_RunThisForMe( uint32_t client, svc_registers const *regs )
{
  register uint32_t h asm ( "r0" ) = client;
  register svc_registers const *r asm ( "r1" ) = regs;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_RunThisForMe)
      , "r" (h)
      , "r" (r)
      : "lr", "cc", "memory" );

  return error;
}

static inline
error_block *Task_ReleaseTask( uint32_t client, svc_registers const *regs )
{
  register uint32_t h asm ( "r0" ) = client;
  register svc_registers const *r asm ( "r1" ) = regs;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_ReleaseTask)
      , "r" (h)
      , "r" (r)
      : "lr", "cc" );

  return error;
}

static inline
error_block *Task_ChangeController( uint32_t client, uint32_t controller )
{
  register uint32_t h asm ( "r0" ) = client;
  register uint32_t replacement asm ( "r1" ) = controller;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_ChangeController)
      , "r" (h)
      , "r" (replacement)
      : "lr", "cc" );

  return error;
}

static inline
error_block *Task_GetRegisters( uint32_t controlled, svc_registers *regs )
{
  register uint32_t h asm ( "r0" ) = controlled;
  register svc_registers *r asm ( "r1" ) = regs;
  register error_block *error asm ( "r0" );

  asm volatile ( "svc %[swi]"
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

  asm volatile ( "svc %[swi]"
             "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OSTask_SetRegisters)
      , "r" (h)
      , "r" (r)
      : "lr", "cc" );

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
  asm volatile ( "svc %[swi]"
             : "=r" (handle)
             : [swi] "i" (OSTask_QueueCreate)
             : "lr", "cc" );

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

  asm volatile ( "svc %[swi]"
             "\n  movvc r3, #0"
             "\n  movvs r3, r0"
             "\n  movvs r0, #0"
      : "=r" (task_handle)
      , "=r" (swi)
      , "=r" (core)
      , "=r" (error)
      : [swi] "i" (OSTask_QueueWait)
      , "r" (handle)
      : "lr", "cc" );

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
  asm volatile ( "svc %[swi]"
      : "=r" (p)
      : [swi] "i" (OSTask_PhysicalFromVirtual)
      , "r" (v)
      , "r" (l)
      : "lr", "cc", "memory" );
  return p;
}

static inline
uint32_t Task_MemoryChanged( void const *va, uint32_t length )
{
  register void const *v asm( "r0" ) = va;
  register uint32_t l asm( "r1" ) = length;
  register uint32_t p asm( "r0" );
  asm volatile ( "svc %[swi]"
      : "=r" (p)
      : [swi] "i" (OSTask_MemoryChanged)
      , "r" (v)
      , "r" (l)
      : "lr", "cc", "memory" );
  return p;
}

// These two can probably have the "lr" clobber removed; they're only
// called from usr32 mode.

// Returns true if lock has been reclaimed by the same task (in
// which case you don't want to release it this time).
// If your code doesn't expect to re-claim the lock, a true result
// probably indicates a serious programming error.
static inline
bool Task_LockClaim( uint32_t *lock, uint32_t handle )
{
/*
  // Considering making change_word_if_equal globally available...
  uint32_t old = change_word_if_equal( lock, 0, handle );
  if (0 != old && handle != (old & ~1)) {
*/
    register uint32_t *p asm( "r0" ) = lock;
    register uint32_t h asm( "r1" ) = handle;
    register bool reclaimed asm( "r0" );
    asm volatile ( "svc %[swi]"
        : "=r" (reclaimed)
        : [swi] "i" (OSTask_LockClaim)
        , "r" (p)
        , "r" (h)
        : "lr", "cc", "memory" );
  return reclaimed;
/*
  }
  return (handle == (old & ~1));
*/
}

// Release a lock, allowing any waiting tasks to claim it.
static inline
void Task_LockRelease( uint32_t *lock )
{
  register uint32_t *p asm( "r0" ) = lock;
  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_LockRelease)
      , "r" (p)
      : "lr", "cc", "memory" );
}

