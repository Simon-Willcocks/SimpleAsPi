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
void call_boot_with_stack_in_high_memory( uint32_t core, void *workspace, uint32_t size );

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

  call_boot_with_stack_in_high_memory( core, (void*) workspace, CORE_WORKSPACE );
}

static __attribute__(( noinline, noreturn ))
void call_boot_with_stack_in_high_memory( uint32_t core, void *workspace, uint32_t size )
{
  // This ensures that the jump to the routine is not relative.
  register void *hi asm( "lr" ) = boot_with_stack;
  register uint32_t c asm( "r0" ) = core;
  register void *w asm( "r1" ) = workspace;
  register uint32_t s asm( "r2" ) = size;

  asm ( "bx lr" : : "r" (hi), "r" (c), "r" (w), "r" (s) );

  __builtin_unreachable();
}

void *memset(void *s, int c, uint32_t n)
{
  uint8_t *p = s;
  for (int i = 0; i < n; i++) { p[i] = c; asm( "" ); }
  return s;
}

// As yet unused...
void set_way_no_CCSIDR2()
{
  asm ( "dsb sy" );
  // Select cache level
  for (int level = 1; level <= 2; level++) {
    uint32_t size;
    asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" ((level-1) << 1) ); // CSSELR Selection Register.
    asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (size) ); // CSSIDR
    uint32_t line_size = ((size & 7)+4);
    uint32_t ways = 1 + ((size & 0xff8) >> 3);
    uint32_t sets = 1 + ((size & 0x7fff000) >> 13);
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

