/* Copyright 2021 Simon Willcocks
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

/* Together with module.script, this routine generates a RISC OS module
 * header.
 *
 * Usage:
 * #define MODULE_CHUNK <chunk number> (not needed if no SWI chunk)
 * #include "module.h"
 * static const uint32_t module_flags = <flags value>; (required)
 *
 * You can then provide implementations for the following, as required.
 *
 * start (function)
 * init (function)
 * finalise (function)
 * service_call (function)
 * title (const char [])
 * help (const char [])
 * keywords (has to be done in assembler, afaics)
 * swi_handler (function)
 * swi_names (const char [])
 * swi_decoder (function)
 * messages_file (const char [])
 *
 * Only include this header file in one C file for each module.
 *
 */
void __attribute__(( naked, section( ".text.init" ) )) file_start()
{
#ifndef MODULE_CHUNK
#define MODULE_CHUNK "0"
#endif
#ifndef CREATION_DATE
#define CREATION_DATE __DATE__
#endif
  asm volatile ( 
  "\nheader:"
  "\n  .word start-header"
  "\n  .word init-header"
  "\n  .word finalise-header"
  "\n  .word service_call-header"
  "\n  .word title-header"
  "\n  .word help-header"
  "\n  .word keywords-header"
  "\n  .word "MODULE_CHUNK
  "\n  .word swi_handler-header"
  "\n  .word swi_names-header"
  "\n  .word swi_decoder-header"
  "\n  .word messages_file-header"
  "\n  .word module_flags-header" );
}

#define NO_start asm( "start = header" )
#define NO_init asm( "init = header" )
#define NO_finalise asm( "finalise = header" )
#define NO_service_call asm( "service_call = header" )
#define NO_title asm( "title = header" )
#define NO_help asm( "help = header" )
#define NO_keywords asm( "keywords = header" )
#define NO_swi_handler asm( "swi_handler = header" )
#define NO_swi_names asm( "swi_names = header" )
#define NO_swi_decoder asm( "swi_decoder = header" )
#define NO_messages_file asm( "messages_file = header" )

#include "CK_types.h"

#define C_CLOBBERED "r0-r3,r12"

#define assert( c ) while (!(c)) { }

static inline
void *__attribute__(( section( ".text.init" ) )) adr( void *fn )
{
  register void *in asm ( "r0" ) = fn;
  register void *in2 asm ( "r1" ) = adr;
  register void *out asm ( "r0" );
  asm ( "add r0, r0, r1"
    "\n  sub r0, r0, #adr-header"
    : "=r" (out) : "r" (in), "r" (in2) );
  return out;
}

// EABI specifies stack is 8-byte aligned at any public interface.
// Created tasks are likely to follow that specification.
uint32_t aligned_stack( void *top )
{
  return ~7 & (uint32_t) top;
}

/* How to declare commands. FIXME: needs a few macros.

asm ( "keywords:"
 "\n  .asciz \"CommandA\""
 "\n  .align"
 "\n  .word command_a_code - header"
 "\n  .word 0xaabbccdd" // dd = min number parameters
                        // bb = max number parameters
                        // cc = GSTrans map for first 8 parameters
                        // aa = Flags
                        //      0x80 Filing system command
                        //      0x40 Match by status/configure
                        //      0x20 Help is code, not a string
 "\n  .word commanda_invalid_status_message-header" // or zero
 "\n  .word commanda_help_text-header" // or zero
<repeat the above for each command>
 "\n  .word 0"
*/

#define C_SWI_HANDLER( cfn ) \
typedef struct { \
  uint32_t r[10]; \
  uint32_t number; \
  struct workspace **private_word; \
} SWI_regs; \
 \
bool __attribute__(( noinline )) cfn( struct workspace *ws, SWI_regs *regs ); \
 \
void __attribute__(( naked, section( ".text.init" ) )) swi_handler() \
{ \
  SWI_regs *regs; \
  register struct workspace **private_word asm( "r12" ); \
  asm volatile ( "push {r0-r9, r11, r12, r14}\n  mov %[regs], sp" : [regs] "=r" (regs), "=r" (private_word) ); \
  if (!cfn( *private_word, regs )) { \
    /* Error: Set V flag */ \
    asm ( "msr cpsr_f, #(1 << 28)" ); \
  } \
  asm volatile ( "pop {r0-r9, r11, r12, pc}" ); \
}

// memset not static, this include file should only be included once in a module;
// The optimiser occasionally uses this routine.
void *memset(void *s, int c, size_t n)
{
  // In this pattern, if there is a larger size, and it is double the current one, use "if", otherwise use "while"
  char cv = c & 0xff;
  char *cp = s;
  // Next size is double, use if, not while
  if ((((size_t) cp) & (1 << 0)) != 0 && n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  uint16_t hv = cv; hv = hv | (hv << (8 * sizeof( cv )));
  uint16_t *hp = (void*) cp;
  // Next size is double, use if, not while
  if ((((size_t) hp) & (1 << 1)) != 0 && n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }

  uint32_t wv = hv; wv = wv | (wv << (8 * sizeof( hv )));
  uint32_t *wp = (void*) hp;
  // Next size is double, use if, not while
  if ((((size_t) wp) & (1 << 2)) != 0 && n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }

  uint64_t dv = wv; dv = dv | (dv << (8 * sizeof( wv )));
  uint64_t *dp = (void*) wp;
  // No larger size, use while, not if, and don't check the pointer bit
  while (n >= sizeof( dv )) { *dp++ = dv; n-=sizeof( dv ); }

  wp = (void *) dp; if (n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }
  hp = (void *) wp; if (n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }
  cp = (void *) hp; if (n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  return s;
}

#include "kernel_swis.h"

static inline void ensure_changes_observable()
{
  asm ( "dsb sy" );
}

static inline void memory_write_barrier()
{
  asm ( "dsb sy" );
}

static inline void memory_read_barrier()
{
  asm ( "dsb sy" );
}

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

static inline void set_VF()
{
  asm ( "msr cpsr_f, #(1 << 28)" );
}

static inline void *rma_claim( uint32_t bytes )
{
  // XOS_Module 6 Claim
  register void *memory asm( "r2" );
  register uint32_t code asm( "r0" ) = 6;
  register uint32_t size asm( "r3" ) = bytes;
  asm ( "svc 0x2001e" : "=r" (memory) : "r" (size), "r" (code) : "lr" );

  return memory;
}

// If the linker complains about "undefined reference to `memcpy'",
// simply include the following instruction in a function that's
// guaranteed not to be optimized out:
//  asm ( "" : : "m" (memcpy) ); // Force a non-line copy of memcpy

static inline
void *memcpy(void *d, void *s, uint32_t n)
{
  uint8_t const *src = s;
  uint8_t *dest = d;
  // Trivial implementation, asm( "" ) ensures it doesn't get optimised
  // to calling this function!
  for (int i = 0; i < n; i++) { dest[i] = src[i]; asm( "" ); }
  return d;
}
