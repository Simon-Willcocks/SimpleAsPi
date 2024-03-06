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

// All features defined in DDI0487C_a_armv8_arm.

// The "ROM" image is allowed to be overwritten from 0x100 to 0x3ff, as
// the Pi firmware does, if allowed.

#include "CK_types.h"
#include "Processor/processor.h"

#ifndef CORE_WORKSPACE
#error "Define CORE_WORKSPACE to be the space needed for each core; stack at top"
#endif

inline uint32_t __attribute__(( always_inline )) get_core_number()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[result], c0, c0, 5" : [result] "=r" (result) );
  return ((result & 0xc0000000) != 0x80000000) ? 0 : (result & 15);
}

static __attribute__(( noinline ))
void set_smp_mode();

static __attribute__(( noinline, noreturn ))
void call_boot_with_stack_in_high_memory( uint32_t core );

// Assumes the image is loaded at location zero.
//  old_kernel=1 in config.txt for all current Pies.

void __attribute__(( naked, noreturn )) _start()
{
  asm (
    "\n.ifne .-_start"
    "\n  .error \"Compiler inserted instructions unexpectedly in _start\""
    "\n.endif"
  );

  // Core 0 is always present, its workspace is always at the top of the
  // initial memory, a core can find the top of the initial memory by
  // adding (core+1) * CORE_WORKSPACE to its core workspace.
  // Or by reading the address of top_of_boot_RAM!
  uint32_t core = get_core_number();

#ifdef DEBUG__SINGLE_CORE
#warning "SINGLE CORE"
  if (core != 0) for (;;) asm ( "wfi" );
#endif

  extern uint8_t top_of_boot_RAM;
  uint32_t top = (uint32_t) &top_of_boot_RAM;
  uint32_t stack_top = top - CORE_WORKSPACE * core;
  uint32_t workspace = stack_top - CORE_WORKSPACE;

  asm ( "mov sp, %[top]" : : [top] "r" (stack_top) );

  create_default_translation_tables( workspace );

  // This is the place to find out what kind of processor this is, and
  // possibly modify the image to provide specific functions for, say,
  // cache maintenance.

  // This has to be done before enabling the caches
  set_smp_mode();

  asm (
    // TTBR0 also includes bits to indicate shareability (bit 1), inner
    // or outer (bit 5), memory cache type (bits 4:3 outer, 6:0 inner).
    "\n  mcr p15, 0, %[one], c13, c0, 1"        // ASID - initial TTBR has all
                                                // memory non-global, so TLBs can
                                                // be cleared by ASID.
    "\n  mcr p15, 0, %[one], c3, c0, 0"         // DACR - Domain 0 master
    "\n  mcr p15, 0, %[zero], c2, c0, 2"        // TTBCR
    "\n  mcr p15, 0, %[ttbr], c2, c0, 0"        // TTBR0 (ttbr0_s)
    "\n  mcr p15, 0, %[sctlr], c1, c0, 0"       // SCTLR (enable MMU)
    "\n  b 0f"
    "\n.balign 0x400" // Skip area that may be overwritten by ATAGs
    "\n0:"
    :
    : [ttbr] "r" (workspace | 0b1001010) // Matches translation table
    , [sctlr] "r" (0x20c5387d)
    , [one] "r" (1)     // Domain and ASID
    , [zero] "r" (0)    // TTBCR
  );

  call_boot_with_stack_in_high_memory( core );
}

static __attribute__(( noinline, noreturn ))
void call_boot_with_stack_in_high_memory( uint32_t core )
{
  static uint32_t lock = 1; // Pre-claimed by core 0

  // The lock is only writable in low memory.
  // The shared workspace is not yet initialised, so we
  // can't assume any word in it is zero.

  uint32_t *plock = (void*) (0xffffff & (uint32_t) &lock);
  if (core_claim_lock( plock, core + 1 )) {
    // "Reclaimed", if arrived here, this must be core 0
    // and other cores are either blocked waiting for the
    // lock or haven't got to it yet.
    memset( &shared, 0, sizeof( shared ) );
  }
  core_release_lock( plock );

  // The shared workspace is cleared for all cores, any locks in it
  // are unclaimed.

  // Clear the core workspace before starting to use it for the stack
  memset( &workspace, 0, sizeof( workspace ) );

  // This ensures that the jump to the routine is not relative.
  register void *hi asm( "lr" ) = boot_with_stack;
  register uint32_t c asm( "r0" ) = core;

  asm ( "mov sp, %[sp]"
    "\n  bx lr"
    :
    : "r" (hi)
    , "r" (c)
    , [sp] "r" ((&workspace.svc_stack)+1) );

  __builtin_unreachable();
}

void *memset(void *s, int c, uint32_t n)
{
  uint8_t *p = s;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
    // for (int i = 0; i < n; i++) { p[i] = c; asm( "" ); }
    // return s;
  if (n < 16) {
    for (int i = 0; i < n; i++) { p[i] = c; asm( "" ); }
  }
  else {
    // No need to check n > 0 in this loop; it starts >= 16
    while (0 != (7 & (uint32_t) p)) { *p++ = c; n--; }

    uint16_t h = c; h = (h << 8) | h;
    uint32_t w = h; w = (w << 16) | w;
    uint64_t d = w; d = (d << 32) | d;
    uint64_t *dp = (void*) p;
    while (n >= sizeof( d )) { *dp++ = d; n -= sizeof( d ); }
    uint32_t *wp = (void*) dp;
    while (n >= sizeof( w )) { *wp++ = w; n -= sizeof( w ); }
    uint32_t *hp = (void*) wp;
    while (n >= sizeof( h )) { *hp++ = h; n -= sizeof( h ); }
    p = (void*) hp;
    if (n > 0) { *p++ = c; }
  }

  return s;
}

void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}

void set_way_no_CCSIDR2()
{
  asm ( "dsb sy" );
  // Select cache level
  for (int level = 1; level <= 2; level++) {
    uint32_t size;
    asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" ((level-1) << 1) ); // CSSELR Selection Register.
    asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (size) ); // CCSIDR
    uint32_t line_size = ((size & 7)+4);
    uint32_t ways = 1 + ((size & 0x1ff8) >> 3);
    uint32_t sets = 1 + ((size & 0xfffe000) >> 13);
    int wayshift; // Number of bits to shift the way index by
    asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways - 1) );

    for (int way = 0; way < ways; way++) {
      uint32_t setway = (way << wayshift) | ((level - 1) << 1);
      for (int set = 0; set < sets; set++) {
        asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
      }
    }
  }

  asm ( "dsb sy" );
}

/*
MRC p15, 1, R0, c0, c0, 1
ANDS R3, R0, #0x07000000
MOV R3, R3, LSR #23
BEQ Finished
MOV R10, #0
Loop1
ADD R2, R10, R10, LSR #1
MOV R1, R0, LSR R2
AND R1, R1, #7
CMP R1, #2
BLT Skip
MCR p15, 2, R10, c0, c0, 0
ISB
MRC p15, 1, R1, c0, c0, 0
AND R2, R1, #7
ADD R2, R2, #4
MOV R4, #0x3FF
ANDS R4, R4, R1, LSR #3
CLZ R5, R4
MOV R9, R4
Loop2
MOV R7, #0x00007FFF
ANDS R7, R7, R1, LSR #13
Loop3
ORR R11, R10, R9, LSL R5
ORR R11, R11, R7, LSL R2
MCR p15, 0, R11, c7, c10, 2
SUBS R7, R7, #1
BGE Loop3
SUBS R9, R9, #1
BGE Loop2
Skip
ADD R10, R10, #2
CMP R3, R10
DSB
BGT Loop1
Finished

void DDI_0487c_a_4831()
{
  asm ( "dsb sy" );

  uint32_t CLIDR;
  asm ( "mrc p15, 1, %[CLIDR], c0, c0, 1" : [CLIDR] "=r" (CLIDR) );
  int cache_level_value = (CLIDR & 0x07000000) >> 23;
  // Strange, given LoC is bits [24:26]
  if (cache_level_value != 0) {
    for (int r10 = 0; r10 < cache_level_value; r10 += 2) {
      enum { None, Instruction, Data, Separate, Unified } type;
      type = ((CLIDR >> (3 * r10 / 2))  & 7);
      switch (type) {
      case None:
      case Instruction: // No need to flush, read only
        break;
      case Data:
      case Separate:
      case Unified:
        {
          asm ( "mcr p15, 2, %[lvl2], c0, c0, 0"
            "\n  isb"
            :
            : [lvl2] "r" (r10) );
          union {
            struct {
              uint32_t size:3; // -4
              uint32_t associativity:10; // -1
              uint32_t num_sets:15; // -1
              uint32_t res0:4;
            };
            uint32_t raw;
          } CCSIDR;
          asm ( "mrc p15, 1, %[CCSIDR], c0, c0, 0" 
            : [CCSIDR] "=r" (CCSIDR.raw) );
          int way_max = CCSIDR.associativity
          int wayshift; // Number of bits to shift the way index by
          asm ( "clz %[ws], %[assoc]"
            : [ws] "=r" (wayshift)
            : [assoc] "r" (ways - 1) );

          for (int way = way_max; way >= 0; way--) {
            for (int set = CCSIDR.num_sets; set >= 0; set--) {
              uint32_t DCCSW = lvl2 | (set << (CCSIDR.size + 4)) | (way << (clz way_max));
              asm ( "mcr p15, 0, %[DCCSW], c7, c10, 2"
                :
                : [DCCSW] "r" (DCCSW) );
            }
          }
        }
        break;
      default: PANIC;
      }
      
      asm ( "dsb sy" );
    }
  }
}
*/

void push_writes_out_of_cache( uint32_t va, uint32_t size )
{
  // TODO: larger sizes probably make a full clean quicker...

  // First, finish any writes to the cache
  asm ( "dsb sy" );

  size += (va & 15);
  va = va & ~15;

  // DCCMVAC Data Cache line Clean by VA to PoC (external RAM)
  for (int i = va; i < va + size; i += 16) {
    asm ( "mcr p15, 0, %[va], c7, c10, 1" : : [va] "r" (i) );
  }

  //set_way_no_CCSIDR2();
}

void RAM_may_have_changed( uint32_t va, uint32_t size )
{
  size += (va & 15);
  va = va & ~15;

  // DCIMVAC Data Cache line Invalidate by VA to PoC (external RAM)
  for (int i = va; i < va + size; i += 16) {
    asm ( "mcr p15, 0, %[va], c7, c6, 1" : : [va] "r" (i) );
  }

  asm ( "dmb sy" );
}

static void Cortex_A7_set_smp_mode()
{
  uint32_t reg;
  asm volatile ( "mrc p15, 0, %[v], c1, c0, 1"
             "\n  orr %[v], %[v], %[b]"
             "\n  mcr p15, 0, %[v], c1, c0, 1"
             : [v] "=&r" (reg) : [b] "ir" (1<<6) );

  asm ( "dsb sy" );
}

static void Cortex_A53_set_smp_mode()
{
  // Write CPU Extended Control Register (64-bits)
  // ARM Cortex-A53 (probably -A72)
  asm ( "mrrc p15, 1, r3, r4, c15"
    "\n  orr r3, r3, #(1 << 6)"
    "\n  mcrr p15, 1, r3, r4, c15" : : : "r3", "r4" );

  asm ( "dsb sy" );
}

static __attribute__(( noinline ))
void set_smp_mode()
{
  uint32_t main_id;

  asm ( "MRC p15, 0, %[id], c0, c0, 0" : [id] "=r" (main_id) );

  switch (main_id) {
  case 0x410fc070 ... 0x410fc07f: Cortex_A7_set_smp_mode(); return;
  case 0x410fd030 ... 0x410fd03f: Cortex_A53_set_smp_mode(); return; // A53
  case 0x410fd080 ... 0x410fd08f: Cortex_A53_set_smp_mode(); return; // A72
  default: for (;;) { asm( "wfi" ); }
  }
}

uint32_t Cortex_A7_number_of_cores()
{
  uint32_t result;
  // L2CTLR, ARM DDI 0500G Cortex-A53, generally usable?
  asm ( "MRC p15, 1, %[result], c9, c0, 2" : [result] "=r" (result) );
  return ((result >> 24) & 3) + 1;
}

uint32_t number_of_cores()
{
  return Cortex_A7_number_of_cores();
}
