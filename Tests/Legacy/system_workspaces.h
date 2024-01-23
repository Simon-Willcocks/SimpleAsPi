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

#include "workspace_ostask.h"
#include "workspace_rawmemory.h"
#include "workspace_mmu.h"
#include "workspace_legacy.h"
#include "workspace_modules.h"

typedef struct {
  uint32_t boot_lock;
  shared_ostask ostask;
  shared_rawmemory rawmemory;
  shared_mmu mmu;
  shared_legacy legacy;
  shared_module module;
} shared_workspace;

typedef struct {
  struct {
    // More than 100 words needed for Legacy subsystem
    // TODO: How much more?
    uint32_t s[400];
  } svc_stack;
  uint32_t core;
  workspace_ostask ostask;
  workspace_rawmemory rawmemory;
  workspace_mmu mmu;
  workspace_legacy legacy;
  workspace_module module;
} core_workspace;

extern shared_workspace shared;
extern core_workspace workspace;
