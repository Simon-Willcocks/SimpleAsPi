#include "CK_types.h"

int printf(const char *format, ...);
void _exit(int status);

#define PANIC do { printf( "Panic at line %d\n", __LINE__ ); _exit( 1 ); } while (false)

static inline bool core_claim_lock( uint32_t *p, int n ) { return false; }
static inline void core_release_lock( uint32_t *p ) { }

#include "../workspace_rawmemory.h"

struct {
  uint32_t core;
  workspace_rawmemory rawmemory;
} workspace;

struct {
  shared_rawmemory rawmemory;
} shared;


