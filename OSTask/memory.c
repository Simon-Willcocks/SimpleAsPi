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

#include "ostask.h"

// This trivial implementation is for rwx memory starting at 0x8000,
// with possible pages for device memory (which I suggest should go
// below 0x8000). There should be no gaps in the virtual memory.

uint32_t top_of( app_memory_block *block )
{
  return (block->va_page + block->pages) << 12;
}

app_memory_block *find_block( OSTaskSlot *slot, uint32_t va )
{
  int i = 0;
  while (slot->app_mem[i].pages != 0
      && slot->app_mem[i].va_page < (va >> 12)) {
    i++;
  }
  if (slot->app_mem[i].pages != 0) {
    for (int j = number_of( slot->app_mem ); j > i; j--) {
      if (slot->app_mem[j-1].pages > 0)
        slot->app_mem[j] = slot->app_mem[j-1];
    }
  }
  return &slot->app_mem[i];
}

uint32_t map_device_pages( uint32_t va,
                           uint32_t page_base,
                           uint32_t pages )
{
  OSTaskSlot *slot = workspace.ostask.running->slot;
  app_memory_block *block = find_block( slot, va );

  block->va_page = va >> 12;
  block->pages = pages;
  block->page_base = page_base;
  block->device = 1;

  return va;
}

uint32_t app_memory_top( uint32_t new )
{
  OSTaskSlot *slot = workspace.ostask.running->slot;
  uint32_t top = 0x8000;
  int i = 0;
  while (slot->app_mem[i].pages != 0
      && slot->app_mem[i].va_page <= (top >> 12)) {
    i++;
    top = top_of( &slot->app_mem[i] );
  }
  if (new != 0) {
    if (new > top) {
      app_memory_block *block = find_block( slot, top );
      uint32_t pages = (new - top) >> 12;

      block->va_page = top >> 12;
      block->pages = pages;
      block->page_base = claim_contiguous_memory( pages );
      block->device = 0;

      top = new;
    }
    else {
      PANIC; // Shrinking app area.
    }
  }

  return top;
}

bool ask_slot( uint32_t va, uint32_t fault )
{
  OSTaskSlot *slot = workspace.ostask.running->slot;
  int i = 0;
  while (slot->app_mem[i].pages != 0
      && slot->app_mem[i].va_page <= (va >> 12)
      && top_of( &slot->app_mem[i] ) < va) {
    i++;
  }
  if (slot->app_mem[i].pages == 0) return false;

  memory_mapping map = {
    .base_page = slot->app_mem[i].page_base,
    .pages = slot->app_mem[i].pages,
    .va = slot->app_mem[i].va_page << 12,
    .type = (slot->app_mem[i].device) ? CK_Device : CK_MemoryRWX,
    .map_specific = 1,
    .all_cores = 0,
    .usr32_access = 1 };

  map_memory( &map );

  return true;
}

void initialise_app_virtual_memory_area()
{
  clear_memory_region( 0, 0x20000000 >> 12, ask_slot );
}

void clear_app_virtual_memory_area( OSTaskSlot *old )
{
  clear_memory_region( 0, 0x20000000 >> 12, ask_slot );
  // Obviously only change the minimum required! FIXME
}

void changing_slot( OSTaskSlot *old, OSTaskSlot *new )
{
  if (old != new) {
    clear_app_virtual_memory_area( old );
    mmu_switch_map( new->mmu_map );
  }
}
