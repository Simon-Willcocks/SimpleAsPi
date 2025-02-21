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
 *
 */

#include "CK_types.h"

// Initialise a heap at the given virtual address and size
void heap_initialise( void *base, uint32_t size );

// Allocate a block of memory of at least size bytes from
// the given heap. Returned address will be on an 8-byte
// (64-bit) boundary.
void *heap_allocate( void *heap, uint32_t size );

// Free a block allocated by heap_allocate
void heap_free( void *base, void const *mem );
