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

// This routine, provided by another subsystem, will be entered with the
// lowest 16MiB of memory mapped at VA 0, and workspace allocated for each
// core. The lowest 16KiB contains a Level 1 Translation Table, the top
// of the workspace is the initial boot stack.

extern uint32_t const boot_mem;

void __attribute__(( noreturn )) boot_with_stack( uint32_t core, 
                                   void *workspace, uint32_t size );

// Memory management:
// 32-bit page numbers can deal with up to 16,384 GiB of RAM

typedef enum {  CK_MemoryRWX,
                CK_MemoryRW,
                CK_MemoryRX,
                CK_MemoryR,
                CK_Device } CK_Memory;

// Handlers either use one of the map_... functions to remove
// the fault or return false if there's nothing it can do.
// (One possible action to remove the fault is to replace the
// running task with one that will not fault. This code doesn't
// have to know about that.)
typedef bool (*memory_fault_handler)( uint32_t va, uint32_t fault );

void clear_memory_region(
                uint32_t *translation_table,
                uint32_t va_base, uint32_t va_pages,
                memory_fault_handler handler );

void map_app_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                CK_Memory memory_type );

void map_global_memory(
                uint32_t *translation_table,
                uint32_t base_page, uint32_t pages, uint32_t va,
                CK_Memory memory_type );


