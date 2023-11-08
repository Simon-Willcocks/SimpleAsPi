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

// To be able to test on 64-bit Linux, use -m32

#include "CK_types.h"
#include "mpsafe_dll.h"
#include "../heap.h"

int printf( char const *fmt, ... );

static uint32_t __attribute__(( aligned( 4096 ) )) test_heap[128] = { };

void show_heap()
{
  for (int i = 0; i < number_of( test_heap ); i+= 8) {
    printf( "\n" );
    for (int j = 0; j < 8; j++) {
      printf( "%08x ", test_heap[i + j] );
    }
  }
  printf( "\n" );
}

#define FAILED { printf( "Failed at line %d\n", __LINE__ ); return 1; }

char *strcpy(char *dest, const char *src);

int main()
{
  printf( "Heap is at %p\n", test_heap );

  heap_initialise( test_heap, sizeof( test_heap ) );
  if (test_heap[0] != 0x50414548) FAILED;

  show_heap();

  void *allocated = heap_allocate( test_heap, 222 );

  printf( "Allocated %p (total: %d)\n", allocated, ((uint32_t*) allocated)[-1] );

  show_heap();

  if (0 != (7 & (uint32_t) allocated)) FAILED;

  strcpy( allocated, "Hello world" );

  show_heap();

  allocated = heap_allocate( test_heap, 12 );

  printf( "Allocated %p (total: %d)\n", allocated, ((uint32_t*) allocated)[-1] );

  show_heap();

  printf( "All passed\n" );

  return 0;
}
