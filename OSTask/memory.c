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
    top = top_of( &slot->app_mem[i] );
    i++;
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
    else if (new < top) {
      PANIC; // Shrinking app area.
    }
  }

  return top;
}

static bool in_range( uint32_t n, uint32_t low, uint32_t above )
{
  return n >= low && n < above;
}

extern uint8_t app_memory_limit;
static uint32_t const app_top = (uint32_t) &app_memory_limit;

extern uint8_t pipes_base;
static uint32_t const pipes_bottom = (uint32_t) &pipes_base;
extern uint8_t pipes_top;
static uint32_t const pipes_end = (uint32_t) &pipes_top;

app_memory_block block_containing( uint32_t va )
{
  app_memory_block result = { 0 }; // Zero length

  OSTaskSlot *slot = workspace.ostask.running->slot;

  if (va < app_top) {
    int i = 0;
    while (slot->app_mem[i].pages != 0
        && !in_range( va,
                      slot->app_mem[i].va_page << 12, 
                      top_of( &slot->app_mem[i] ) )
        && i < number_of( slot->app_mem )) {
      i++;
    }
    if (slot->app_mem[i].pages == 0) {
#ifdef DEBUG__LOG_SLOT_MEMORY
Task_LogString( "Failed to match VA ", 0 );
Task_LogHex( va );
Task_LogString( " in ", 0 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogNewLine();
#endif
    }
    result = slot->app_mem[i];
  }
  else if (va >= pipes_bottom && va < pipes_end) {
    int i = 0;
    // Double-mapping is taken care of in insert_pipe_in_gap, which
    // creates two entries with the same physical address.

    while (slot->pipe_mem[i].pages != 0
        && !in_range( va,
                      slot->pipe_mem[i].va_page << 12, 
                      top_of( &slot->pipe_mem[i] ) )
        && i < number_of( slot->pipe_mem )) {

      i++;
    }
#ifdef DEBUG__LOG_SLOT_MEMORY
    if (!in_range( va,
         slot->pipe_mem[i].va_page << 12, 
         top_of( &slot->pipe_mem[i] ) )) {
Task_LogString( "Failed to match VA (pipe) ", 0 );
Task_LogHex( va );
Task_LogString( " in ", 0 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogNewLine();
    }
#endif
    result = slot->pipe_mem[i];
  }
  else {
    memory_pages global_area = walk_global_tree( va );

    if (global_area.number_of_pages != 0) {
      result.pages = global_area.number_of_pages;
      if (0 != (0xfff & global_area.virtual_base)) PANIC;
      result.va_page = global_area.virtual_base >> 12;
      result.page_base = global_area.base_page;
    }
#ifdef DEBUG__LOG_SLOT_MEMORY
    else {
Task_LogString( "VA not in app, pipe or global memory! ", 0 );
Task_LogHex( va );
Task_LogString( " in ", 0 );
Task_LogHex( (uint32_t) workspace.ostask.running );
Task_LogString( " on core ", 0 );
Task_LogSmallNumber( workspace.core );
Task_LogNewLine();
    }
#endif
  }

  return result;
}

bool ask_slot( uint32_t va, uint32_t fault )
{
  app_memory_block block = block_containing( va );

  if (block.pages == 0) {
    return false;
  }

  memory_mapping map = {
    .base_page = block.page_base,
    .pages = block.pages,
    .va = block.va_page << 12,
    .type = (block.device) ? CK_Device : CK_MemoryRWX,
    .map_specific = 1,
    .all_cores = 0,
    .usr32_access = 1 };

  map.not_shared = 0; // Set to one if only one task in this slot FIXME

  map_memory( &map );

  return true;
}

uint8_t pipes_base;
uint8_t pipes_top;

void initialise_app_virtual_memory_area()
{
  clear_memory_region( 0, app_top >> 12, ask_slot );
  clear_memory_region( (&pipes_base - (uint8_t*) 0), 
                       (&pipes_top - &pipes_base) >> 12, ask_slot );
}

void clear_app_virtual_memory_area( OSTaskSlot *old )
{
  uint32_t top = app_memory_top( 0 );
  top += 0xfffff; // FIXME: Bug in clear_memory_region?
  clear_memory_region( 0, top >> 12, ask_slot );
  clear_memory_region( (&pipes_base - (uint8_t*) 0), 
                       (&pipes_top - &pipes_base) >> 12, ask_slot );
  // Obviously only change the minimum required! FIXME

  // The scratch space is a pain in the ass. The RO code really doesn't
  // want it moved.

  uint32_t volatile *p = &shared.mmu.legacy_scratch_space;
  if (2 > *p) {
    if (0 == change_word_if_equal( p, 0, 1 )) {
      *p = claim_contiguous_memory( 4 );
      if (*p == 0) PANIC;
    }
    else {
      while (*p == 1) {}
    }
  }

  memory_mapping map = {
    .base_page = shared.mmu.legacy_scratch_space,
    .pages = 4,
    .va = 0x4000,
    .type = CK_MemoryRWX,
    .map_specific = 0,
    .all_cores = 1,
    .usr32_access = 0 };

  map.not_shared = 0; // Set to one if only one task in this slot FIXME

  map_memory( &map );
}

void map_first_slot()
{
  OSTaskSlot *slot = shared.ostask.first;
  initialise_app_virtual_memory_area();
  mmu_switch_map( slot->mmu_map );
  workspace.ostask.currently_mapped = slot;
}

void map_slot( OSTaskSlot *new )
{
  OSTaskSlot *current = workspace.ostask.currently_mapped;
#ifdef DEBUG__UDF_ON_MAP_SLOT
  asm( "udf 2" : : "r" (current) );
#endif

  // TODO: ISTR it's possible to disable table walks (get an exception on
  // TLB miss); could do that here, and only update the map if the ASID
  // has changed since the last time it was enabled.
  if (current != new) {
    clear_app_virtual_memory_area( current );
    mmu_switch_map( new->mmu_map );
    workspace.ostask.currently_mapped = new;
  }
}
