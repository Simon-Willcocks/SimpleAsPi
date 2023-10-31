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

// All values in physical PAGES!!! 1MiB = 0x100.

// Panics if any part of block is already free!
void free_contiguous_memory( uint32_t base, uint32_t pages );

// Returns -1 if unavailable
#define contiguous_memory_unavailable 0xffffffff
uint32_t claim_contiguous_memory( uint32_t pages );

// TODO: Include alignment, or simply allocate pages + alignment and
// free the leftover?
