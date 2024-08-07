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

typedef struct {
} workspace_mmu;

typedef union l2tt l2tt;

typedef struct {
  uint32_t lock;
  l2tt *free;
  uint32_t l2tables_phys_base; // page index
  uint32_t legacy_scratch_space; // TOTALLY IN THE WRONG PLACE, but I want something to work!
} shared_mmu;

