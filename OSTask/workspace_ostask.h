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

typedef struct OSTask OSTask;
typedef struct OSTaskSlot OSTaskSlot;
typedef struct OSPipe OSPipe;
typedef union OSQueue OSQueue;

typedef struct {
  OSTask *running;
  OSTask *idle;
  OSPipe *log_pipe;
  OSTaskSlot *currently_mapped;
  OSTask *irq_task;

  struct {
    uint32_t stack[64];
  } fiq_stack;

  struct {
    uint32_t stack[64];
  } irq_stack;

  struct {
    uint32_t stack[64];
  } und_stack;

  struct {
    uint32_t stack[128];
  } abt_stack;
} workspace_ostask;

typedef struct {
  uint32_t lock;        // Used for boot and for when manipulating blocked
  uint32_t pipes_lock;
  OSPipe *pipes;

  OSTask *runnable;
  OSTask *sleeping;
  OSTask *blocked;      // Claiming a lock

  OSTaskSlot *first;

  OSTask *task_pool;
  OSTaskSlot *slot_pool;
  OSQueue *queue_pool;
  OSPipe *pipe_pool;

  uint32_t number_of_cores;
  OSTask *moving;       // List of tasks wanting to run on a specific core
                        // This list should almost always be empty and always
                        // be short.

  mmu_page *last_global_device;

  uint32_t queues_lock;
} shared_ostask;

