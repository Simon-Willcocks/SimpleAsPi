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

#include "workspace_mmu.h"

typedef struct example example;

struct example {
  example *next;
  example *prev;
  uint32_t core;
};

typedef struct {
  uint32_t boot_lock;
  shared_mmu mmu;
  example *list;

  example entries[4000];
} shared_workspace;

typedef struct {
  uint32_t core;
  struct {
    uint32_t s[100];
  } svc_stack;
  workspace_mmu mmu;
} core_workspace;

extern shared_workspace shared;
extern core_workspace workspace;
