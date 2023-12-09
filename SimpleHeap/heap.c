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
#include "processor.h"
#include "heap.h"

typedef struct free_heap_block free_heap_block;
typedef struct heap_block heap_block;
typedef struct heap heap;

// Note: Sizes in these structures include the size of the
// structure it's in, so ((uint8_t*) b)[b->size] is the byte
// above the block (free or used).

struct __attribute__(( packed, aligned( 4 ) )) free_heap_block {
  free_heap_block *next, *prev;
  uint32_t size;
};

static inline void *free_block_end( free_heap_block *b )
{
  uint32_t start = (uint32_t) b;
  return (void*) (start + b->size);
}

struct __attribute__(( aligned( 4 ), packed )) heap_block {
  uint32_t magic;
  uint32_t size;
};

static inline void *block_end( heap_block *b )
{
  uint32_t start = (uint32_t) b;
  return (void*) (start + b->size);
}

struct __attribute__(( aligned( 4 ), packed )) heap {
  uint32_t magic;
  free_heap_block *free;
};

#include "mpsafe_dll.h"

MPSAFE_DLL_TYPE( free_heap_block );

static const uint32_t magic_heap = 0x50414548; // "HEAP"
static const uint32_t magic_used = 0x44455355; // "USED"

void heap_initialise( void *base, uint32_t size )
{
  if (sizeof( heap ) != 8) PANIC;
  if (sizeof( free_heap_block ) != 12) PANIC;
  if (sizeof( heap_block ) != 8) PANIC;

  heap *h = base;
  h->magic = magic_heap;
  h->free = (void*) (h + 1);

  dll_new_free_heap_block( h->free );
  h->free->size = size - sizeof( heap );
}

static inline free_heap_block *first_fit( free_heap_block **head, void *p )
{
  // Size has already been adjusted for header size;
  // allocate exactly that many bytes (or a bit more if the
  // free block would be left with a ridiculously tiny size)
  uint32_t size = (uint32_t) p;

  if (*head == 0) return 0;

  free_heap_block *f = *head;

  do {
    if (f->size > size && f->size <= size + 32) {
      // Why 32? A guess. Changing it will not make much of an impact.
      size = f->size;
    }
    if (f->size == size) {
      PANIC; // remove from list...
    }
    else if (f->size > size) {
      uint8_t *end = free_block_end( f );
      heap_block *b = (void*) (end - size);
      b->magic = magic_used;
      b->size = size;

      if (free_block_end( f ) != block_end( b )) PANIC;

      f->size -= size;

      return (void*) (b + 1); // The byte after the header, where the user can write
    }
    f = f->next;
  } while (f != *head);

  return 0;
}

void *heap_allocate( void *base, uint32_t size )
{
  heap *h = base;
  if (h->magic != magic_heap) PANIC;

  // Allocate a multiple of 16 bytes, including the header.
  size = (size + sizeof( heap_block ) + 15) & ~15;

  free_heap_block *result = mpsafe_manipulate_free_heap_block_list_returning_item( &h->free, first_fit, (void*) size );
  return result;
}

void heap_free( void *base, void *mem )
{
  // FIXME: Serious memory leak!
  // Like, all of it!
}
