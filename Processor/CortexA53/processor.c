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

#ifndef CORE_WORKSPACE
#error "Define CORE_WORKSPACE to be the space needed for each core; stack at top"
#endif

void __attribute__(( noreturn )) boot_with_stack( uint32_t core, void *workspace, uint32_t size );

uint32_t const boot_mem = 16 << 20;

inline uint32_t __attribute__(( always_inline )) get_core_number()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[result], c0, c0, 5" : [result] "=r" (result) );
  return ((result & 0xc0000000) != 0x80000000) ? 0 : (result & 15);
}

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
  uint32_t core = get_core_number();

  uint32_t stack_top = boot_mem - CORE_WORKSPACE * core;
  uint32_t workspace = stack_top - CORE_WORKSPACE;

  uint32_t *tt = (void*) workspace;
  uint32_t section_flags = 0x00075c06;

// Note: ROM segments: 75c06 =>
// Least to most significant bits:
//      Type 2 (segment)
//      B = 1   Since TEX[2] == 1:
//      C = 0     Write back, Read allocate, Write Allocate (inner)
//      XN = 0 (executable)
//      Domain = 0 (basically obsolete)
//      P = 0
//      AF = 0 (Don't tell me when first accessed)
//      Unprivileged access = 1 (but only until boot)
//      TEX = 5 => Write back, Read allocate, Write Allocate (outer)
//      Read-write
//      Shared
//      ASID-associated (not Global)
//      Supersection
//      Secure

// I had misunderstood the meaning of the top bits; all entries that
// are for a supersection must have the same bits 20-31, bits 20-23
// are used for the physical address bits above the normal 32-bit
// address space. G4.4

  // FIXME: only works for multiples of 16MiB
  for (int i = 0; i < boot_mem >> 20; i++) {
    tt[i] = ((i << 20) & 0xff000000) | section_flags;
  }

  asm (
        "mov sp, %[top]"
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
    , [top] "r" (stack_top)
    , [sctlr] "r" (0x20c5387d)
    , [one] "r" (1)     // Domain and ASID
    , [zero] "r" (0)    // TTBCR
  );

  boot_with_stack( core, (void*) workspace, CORE_WORKSPACE );
}

void *memset(void *s, int c, uint32_t n)
{
  uint8_t *p = s;
  for (int i = 0; i < n; i++) { p[i] = c; }
  return s;
}

