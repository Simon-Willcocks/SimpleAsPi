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
} workspace_rawmemory;

typedef struct {
  uint32_t lock;

  // One bit per section
  uint32_t sections[4096/32]; // Maximum 4GiB

  // This array holds free pages of memory
  struct {
    uint32_t base:24; // Feel free drop the bitfield numbers when
    uint32_t count:8; // using more than 64GiB of physical RAM
  } early_released_pages[32];
} shared_rawmemory;

// Later implementation:
// separate lists of 4, 8, 16... 512KiB blocks, map first page of
// each into RAM, and fill with the addresses of the others...
