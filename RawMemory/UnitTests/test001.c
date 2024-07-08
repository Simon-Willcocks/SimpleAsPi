
#include "processor.h"
#include "raw_memory_manager.h"

void *memset(void *s, int c, long unsigned int n);

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

void show_bits()
{
  uint32_t *p = &shared.rawmemory.sections[0];
  for (int i = 0; i < 4096 / 16; i += 8) {
    printf( "%08x %08x %08x %08x  %08x %08x %08x %08x\n",
      p[i], p[i+1], p[i+2], p[i+3],
      p[i+4], p[i+5], p[i+6], p[i+7] );
  }
}

int main()
{
  if (count_leading_zeros( 0 ) != 32
   || count_leading_zeros( 0x80000000 ) != 0
   || count_leading_zeros( 0x99999999 ) != 0) {
    printf( "count_leading_zeros error\n" );
  }
  if (count_leading_ones( 0 ) != 0
   || count_leading_ones( 0x80000000 ) != 1
   || count_leading_ones( 0xf0000000 ) != 4
   || count_leading_ones( 0x99999999 ) != 1) {
    printf( "count_leading_ones error\n" );
  }
  uint32_t result = 0;
  for (uint32_t n = 0xffffffff; n != 0; n = n >> 1) {
    if (result != count_leading_zeros( n )) {
      printf( "count_leading_zeros error\n" );
    }
    else
      result++;
  }

  uint32_t *p = &shared.rawmemory.sections[0];
  uint32_t *q;

  free_contiguous_memory( 0x2000, 0x6000 );
  q = &p[0];
  if (q[0] != 0 || q[1] != 0xffffffff || q[2] != 0xffffffff || q[3] != 0xffffffff || q[4] != 0) goto fail;

  free_contiguous_memory( 0x8800, 0x5800 );
  q = &p[4];
  if (q[0] != 0x00ffffff || q[1] != 0xffffffff || q[2] != 0xffffffff || q[3] != 0) goto fail;

  free_contiguous_memory( 0x10800, 0x4000 );
  q = &p[8];
  if (q[0] != 0x00ffffff || q[1] != 0xffffffff || q[2] != 0xff000000 || q[3] != 0) goto fail;

  free_contiguous_memory( 0x18000, 0x6200 );
  q = &p[12];
  if (q[0] != 0xffffffff || q[1] != 0xffffffff || q[2] != 0xffffffff || q[3] != 0xc0000000) goto fail;

  free_contiguous_memory( 0x20000, 0x0100 );
  q = &p[16];
  if (q[0] != 0x80000000) goto fail;

  free_contiguous_memory( 0x20300, 0x0200 );
  if (q[0] != 0x98000000) goto fail;

  free_contiguous_memory( 0x20700, 0x0100 );
  if (q[0] != 0x99000000) goto fail;

  free_contiguous_memory( 0x20100, 0x0100 );
  if (q[0] != 0xd9000000) goto fail;

  printf( "All passed!\n" );

fail:
  show_bits();

  memset( p, 0, sizeof( shared.rawmemory.sections ) );
  free_contiguous_memory( 0x200, 0x100 );
  printf( "\n" );

  if (0x200 != claim_contiguous_memory( 0x100 )) {
    printf( "Failed\n" );
    show_bits();
  }

  free_contiguous_memory( 0x1002, 0x8 );
  uint32_t claimed = claim_contiguous_memory( 8 );
  if (claimed != 0x1002) printf( "failed\n" );

  // Put in 512 pages
  free_contiguous_memory( 0x1000, 0x200 );
  // Take out 512 pages...
  for (int i = 0; i < 512; i++) {
    if (claim_contiguous_memory( 1 ) == 0xffffffff) {
      printf( "Failed\n" );
      break;
    }
  }

  // No more pages?
  if (claim_contiguous_memory( 1 ) != 0xffffffff) {
    printf( "Failed\n" );
  }

  free_contiguous_memory( 0x2000, 0x4000 );
  if (0 == claim_contiguous_memory( 0x400 )) {
    printf( "Failed\n" );
  }
  if (0 == claim_contiguous_memory( 0x100 )) {
    printf( "Failed\n" );
  }
  if (0 == claim_contiguous_memory( 0x400 )) {
    printf( "Failed\n" );
  }
  if (0 == claim_contiguous_memory( 0x400 )) {
    printf( "Failed\n" );
  }
  if (0 == claim_contiguous_memory( 0x400 )) {
    printf( "Failed\n" );
  }
  show_bits();

  return 0;
}
