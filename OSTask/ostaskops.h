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
  , OSTask_RegisterInterruptSources     // Once only
  , OSTask_EnablingInterrupt            // Disable IRQs while I get
  , OSTask_WaitForInterrupt             // ready to wait.
  , OSTask_InterruptIsOff               // Enable interrupts (I'm done,
                                        // but not yet waiting)
  , OSTask_SwitchToCore                 // Use sparingly!
  , OSTask_MapDevicePages               // For device drivers
  , OSTask_AppMemoryTop                 // r0 = New value, or 0 to read
  , OSTask_RunThisForMe                 // Run code in the context of the task
  , OSTask_GetRegisters                 // Get registers of controlled task
  , OSTask_SetRegisters                 // Set registers of controlled task
  , OSTask_RelinquishControl            // Resume controlling task
  , OSTask_ReleaseTask                  // Resume controlled task
  , OSTask_Tick                         // For HAL module use only

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
  , OSTask_QueueWaitCore
  , OSTask_QueueWaitSWI
  , OSTask_QueueWaitCoreAndSWI
};

static inline
void Task_Sleep( uint32_t ms )
{
#ifdef QEMU
  ms = ms / 10;
#endif
  register uint32_t t asm( "r0" ) = ms;
  // sets r0 to 0. volatile is needed, since the optimiser ignores
  // asm statements with an output which is ignored.
  asm volatile ( "svc %[swi]" : "=r" (t) : [swi] "i" (OSTask_Sleep), "r" (t) );
}

static inline
void Task_EndTask()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_EndTask)
    : "lr", "cc" );
}

static inline
void Task_RegisterInterruptSources( uint32_t n )
{
  register uint32_t number asm ( "r0" ) = n;

  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_RegisterInterruptSources)
    , "r" (number)
    : "lr", "cc" );
}

// Note: base page is physical address >> 12
static inline
void *Task_MapDevicePages( uint32_t va, uint32_t base_page, uint32_t pages )
{
  register uint32_t virt asm ( "r0" ) = va;
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
void Task_EnablingIntterupt()
{
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_EnablingInterrupt)
    : "lr", "cc" );
}

static inline
void Task_WaitForInterrupt( uint32_t n )
{
  register uint32_t number asm ( "r0" ) = n;

  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_WaitForInterrupt)
    , "r" (number)
    : "lr", "cc" );
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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline
PipeSpace PipeOp_DataConsumed( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  // In this case, the bytes are the number of bytes no longer of interest.
  // The returned information is the same as from WaitForData and indicates the remaining
  // data after the consumed bytes have been removed. The virtual address of the remaining
  // data may not be the same as the address of the byte after the last consumed byte.
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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
        : "lr", "cc", "memory"
        );

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
      : "lr", "cc", "memory" );

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
      : "lr" );

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
      : "lr" );

  return error;
}

typedef struct {
  uint32_t task_handle;
  uint32_t swi;
  uint32_t core;
  error_block *error;
} queued_task;

#ifndef NOT_DEBUGGING
static inline
#endif
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

