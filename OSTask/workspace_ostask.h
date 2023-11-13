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
typedef struct OSQueue OSQueue;

typedef struct {
  OSTask *running;
  OSTask *idle;
  OSPipe *debug_pipe;
  OSTaskSlot *currently_mapped;

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
  uint32_t lock;
  uint32_t pipes_lock;
  OSPipe *pipes;

  OSTask *runnable;
  OSTask *sleeping;

  OSTaskSlot *first;

  OSTask *task_pool;
  OSTaskSlot *slot_pool;

  uint32_t number_of_interrupt_sources;
  uint32_t number_of_cores;
  OSTask **irq_tasks;   // 2-D array of tasks handling interrupts,
                        // number of cores x number of sources

  uint32_t queues_lock;
} shared_ostask;

