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

#include "CK_types.h"
#include "processor.h"
#include "raw_memory_manager.h"

static inline bool section_aligned( uint32_t base_page )
{
  return 0 == (base_page & 0xff);
}

// Panics if any part of block is already free!
void free_contiguous_memory( uint32_t base, uint32_t pages )
{
  // workspace.rawmemory.sections is an array of bit fields, one for
  // each MiB.
  //   0 Allocated
  //   1 Free
  // Same for split sections, 
  bool reclaimed = core_claim_lock( &shared.rawmemory.lock, workspace.core+1 );

  if (!section_aligned( base ) || !section_aligned( pages )) {
    // Involves pages, possibly sections too.

    // TODO check that the containing sections are allocated

    // If block spans a section boundary, split request into 2 or 3
    if ((base >> 8) != ((base + pages) >> 8)) {
      if (0 != (base & 0xff)) {
        uint32_t in_first_section = 0x100 - (base & 0xff);
        free_contiguous_memory( base, in_first_section );
        base += in_first_section; pages -= in_first_section;
      }
      if (0 != (base & 0xff)) PANIC;
      if (pages > 0xff) {
        // Free whole sections
        uint32_t number_of_sections = pages >> 8;
        uint32_t number_of_pages = number_of_sections << 8;
        free_contiguous_memory( base, number_of_pages );
        base += number_of_pages; pages -= number_of_pages;
      }
      if (pages > 0xff) PANIC;
    }

    if (pages != 0) {
      // Here, we have to do the work of freeing pages.
      // There are fewer than 256, all in the same section
      int i = 0;
      while (i < number_of( shared.rawmemory.early_released_pages )) {
        if (0 == shared.rawmemory.early_released_pages[i].count) {
          shared.rawmemory.early_released_pages[i].count = pages;
          shared.rawmemory.early_released_pages[i].base = base;
          break;
        }
        i++;
      }
      if (i == number_of( shared.rawmemory.early_released_pages )) {
        PANIC; // TODO Split a section!
      }
    }
  }
  else {
    // Releasing sections only
    uint32_t *sections = shared.rawmemory.sections;

    uint32_t section = base >> 8;
    uint32_t count = pages >> 8;

    uint32_t first_index = section / 32;
    uint32_t last_index = (section + count) / 32;

    uint32_t in_first_word = section & 31;
    uint32_t in_last_word = (section + count) & 31;

    uint32_t first_bits = 0xffffffff >> in_first_word;
    uint32_t last_bits = ~(0xffffffff >> in_last_word);

    if (first_index == last_index) {
#ifdef DEBUG__CYNICAL_RAW_MEMORY
      if (0 != (sections[first_index] & (first_bits & last_bits))) PANIC;
#endif
      sections[first_index] |= (first_bits & last_bits);
    }
    else {
#ifdef DEBUG__CYNICAL_RAW_MEMORY
      if (0 != (sections[first_index] & first_bits)) PANIC;
#endif
      sections[first_index++] |= first_bits;
      if (last_bits != 0) {
#ifdef DEBUG__CYNICAL_RAW_MEMORY
        if (0 != (sections[last_index] & last_bits)) PANIC;
#endif
        sections[last_index] |= last_bits;
      }
      while (first_index < last_index) {
#ifdef DEBUG__CYNICAL_RAW_MEMORY
        if (0 != sections[first_index]) PANIC;
#endif
        sections[first_index++] = 0xffffffff;
      }
    }
  }
  if (!reclaimed) core_release_lock( &shared.rawmemory.lock );
}

static inline uint32_t count_leading_zeros( uint32_t v )
{
  uint32_t result = 0;
  if (v == 0) return 32;

  int32_t sv = (int32_t) v;
  while ((sv << result) > 0) { result++; }

  return result;
}

static inline uint32_t count_leading_ones( uint32_t v )
{
  return count_leading_zeros( ~v );
}

// Returns -1 if unavailable
uint32_t claim_contiguous_memory( uint32_t pages )
{
  uint32_t result = contiguous_memory_unavailable;

  bool reclaimed = core_claim_lock( &shared.rawmemory.lock, workspace.core+1 );

  if (section_aligned( pages )) {
    uint32_t const limit = number_of( shared.rawmemory.sections );
    uint32_t *sections = shared.rawmemory.sections;
    uint32_t required = pages >> 8;

    if (required >= 32) PANIC; // TODO: allocate more than 32MiB at most

    int i = 0;
    while (i < limit && result == contiguous_memory_unavailable) {
      uint32_t section = sections[i];
      uint32_t l0 = count_leading_zeros( section );
      while (l0 < 32 && result == contiguous_memory_unavailable) { // non-zero
        uint32_t l1 = count_leading_ones( section << l0 );

        // l1 free sections in a row. Enough?
        if (l1 >= required) {
          result = ((i * 32) + l0) << 8;
          uint32_t mask = 0xffffffff;
          mask = mask << (32 - required);
          mask = mask >> l0;
          sections[i] &= ~mask;
        }
        else {
          // That wasn't enough in a row, but there could still be,
          // e.g. 0x01100171 should match 3 MiB.
          l0 += l1 + count_leading_zeros( section << (l0 + l1) );
        }
      }
      i++;
    }
  }
  else {
    int i = 0;
    int empty_entry = -1;
    while (i < number_of( shared.rawmemory.early_released_pages )
        && pages > shared.rawmemory.early_released_pages[i].count) {
      if (shared.rawmemory.early_released_pages[i].count == 0) empty_entry = i;
      i++;
    }
    if (pages >= 0x100) PANIC; // TODO: allow result to span sections
    if (i >= number_of( shared.rawmemory.early_released_pages )) {
      // Salvagable?
      if (empty_entry != -1) {
        uint32_t section = claim_contiguous_memory( 0x100 );
        if (section != 0xffffffff) {
          shared.rawmemory.early_released_pages[empty_entry].base = section + pages;
          shared.rawmemory.early_released_pages[empty_entry].count = 0x100 - pages;
          result = section;
        }
      }
    }
    else {
      result = shared.rawmemory.early_released_pages[i].base;
      shared.rawmemory.early_released_pages[i].base += pages;
      shared.rawmemory.early_released_pages[i].count -= pages;
    }
  }

  if (!reclaimed) core_release_lock( &shared.rawmemory.lock );

  return result;
}
