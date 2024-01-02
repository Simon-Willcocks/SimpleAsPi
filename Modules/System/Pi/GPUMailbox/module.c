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

#include "CK_types.h"
#include "ostaskops.h"

typedef struct workspace workspace;

#define MODULE_CHUNK "0x1080"

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

#include "module.h"

NO_start;
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

const char title[] = "PiMailbox";
const char help[] = "PiMailbox\t0.01 (" CREATION_DATE ")";

static inline void push_writes_to_device()
{
  asm ( "dsb" );
}

#include "bcm_gpu.h"

static uint32_t const gpu_page = 0x3f00b;

static GPU volatile *const gpu = (void*) 0x1000;

typedef struct {
  uint32_t task;
  uint32_t request_address;
} outstanding_request;

#define MAX_REQUESTS 16

struct workspace {
  outstanding_request request[MAX_REQUESTS];
  uint32_t queue;
  struct {
    uint32_t stack[63];
  } response_manager_stack;
  struct {
    uint32_t stack[64];
  } mailbox_manager_stack;
};

// Choices: 

// Clients must ensure request block is in contiguous memory or
// server copies them to such.  Hell with it, they can do the work; 
// the response will turn up in their memory space, copying is a 
// waste of time.

// Clients can make multiple requests on one channel, or not. Not yet.

// Channel encoded in SWI or register? SWI, it's not going to be a
// run-time question.

static inline bool receive_empty( GPU_mailbox volatile *m )
{
  return 0 != (m->status & (1 << 30));
}

static inline bool transmit_full( GPU_mailbox volatile *m )
{
  return 0 != (m->status & (1 << 31));
}

void response_manager( uint32_t handle, workspace *ws )
{
  Task_LogString( "response_manager\n", 0 );

  Task_EnablingInterrupts();

  gpu->mailbox[0].config = 1;

  for (;;) {
    // Wait for GPU interrupt - move to GPU module
    Task_LogString( "Waiting for GPU interrupt\n", 0 );

    register uint32_t num asm ( "r0" ) = 8;
    asm ( "svc 0x1000" : : "r" (num) );

    Task_LogString( "GPU interrupt\n", 0 );

    while (!receive_empty( &gpu->mailbox[0] )) {
      uint32_t response = gpu->mailbox[0].value;
      bool found = false;

      for (int i = 0; i < MAX_REQUESTS && !found; i++) {
        outstanding_request req = ws->request[i];

        found = req.task != 0 && req.request_address == response;

        if (found) {
          // Free the entry
          ws->request[i].task = 0;
          Task_ReleaseTask( req.task, 0 );
        }
      }
      if (!found) {
        Task_LogString( title, sizeof( title ) - 1 );
        Task_LogString( ": Unexpected response ", 0 );
        Task_LogHex( response );
        Task_LogString( ", dropped\n", 0 );
      }
    }
  }
}

void mailbox_manager( uint32_t handle, workspace *ws )
{
  Task_LogString( "mailbox_manager\n", 0 );

  Task_MapDevicePages( gpu, gpu_page, 1 );

  register void *start asm( "r0" ) = response_manager;
  register uint32_t sp asm( "r1" ) = aligned_stack( &ws->response_manager_stack + 1 );
  register workspace *r1 asm( "r2" ) = ws;
  register uint32_t new_handle asm( "r0" );
  asm ( "svc %[swi]"
    : "=r" (new_handle)
    : [swi] "i" (OSTask_Create)
    , "r" (start)
    , "r" (sp)
    , "r" (r1)
    : "lr", "cc" );

  uint32_t response_task = new_handle;

  for (;;) {
    Task_LogString( "GPUMailbox waiting for request\n", 0 );

    queued_task client = Task_QueueWait( ws->queue );

    Task_LogString( "GPUMailbox request received for channel ", 0 );
    Task_LogSmallNumber( 0xf & client.swi );
    Task_LogNewLine();

    svc_registers regs;

    Task_GetRegisters( client.task_handle, &regs );

    uint32_t req_pa = regs.r[0];

    assert( (req_pa & 15) == 0 );

    // Get physical address of request block - check!

    req_pa |= 0xf & client.swi;
    // Merge with channel number 0..16 - check!

    // Put task into client list

    // mailbox_manager can change a request's task from 0 to non-zero,
    // response_manager can change a request's task from non-zero to zero.
    // IOW, there's no chance of a task handle changing unexpectedly (the
    // request_address is only ever written by mailbox_manager, and only
    // before the request is passed to the GPU).

    // FIXME: Handle the case of too many requests
    // Set r0 to error, spsr VF, and release, OR block waiting
    // for the response_manager to release one.

    outstanding_request *req = 0;
    for (int i = 0; i < MAX_REQUESTS && 0 == req; i++) {
      if (0 == ws->request[i].task) {
        req = &ws->request[i];
      }
    }

    if (req == 0) {
      asm ( "bkpt 1" );
    }

    req->task = client.task_handle;
    req->request_address = req_pa;

    // Transfer control of client to reception task - check
    Task_ChangeController( client.task_handle, response_task );

    // Send to mailbox
    gpu->mailbox[1].value = req_pa;

    // If mailbox full, wait for not-full interrupt before
    // TODO
    // waiting for another request.
  }
}

void __attribute__(( noinline )) c_init( workspace **private,
                                         char const *env,
                                         uint32_t instantiation )
{
  *private = rma_claim( sizeof( workspace ) );
  workspace *ws = *private;

  memset( ws, 0, sizeof( workspace ) );

  ws->queue = Task_QueueCreate();

  swi_handlers handlers = { };
  for (int i = 0; i < 16; i++) {
    handlers.action[i].queue = ws->queue;
  }

  Task_RegisterSWIHandlers( &handlers );

  register void *start asm( "r0" ) = mailbox_manager;
  register uint32_t sp asm( "r1" ) = aligned_stack( &ws->mailbox_manager_stack + 1 );
  register workspace *r1 asm( "r2" ) = ws;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OSTask_Spawn)
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

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}

