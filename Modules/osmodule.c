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
#include "kernel_swis.h"

typedef struct module_header module_header;

typedef struct {
  char const *name;
  module_header *start;
} rom_module;

#include "rom_modules.h"

DEFINE_ERROR( ModuleNotFound, 0x888, "Module not found" );
DEFINE_ERROR( NoRoomInRMA, 0x888, "No room in RMA" );
DECLARE_ERROR( UnknownSWI );

extern uint32_t LegacyModulesList;

struct module_header {
  uint32_t offset_to_start;
  uint32_t offset_to_initialisation;
  uint32_t offset_to_finalisation;
  uint32_t offset_to_service_call_handler;
  uint32_t offset_to_title_string;
  uint32_t offset_to_help_string;
  uint32_t offset_to_help_and_command_keyword_table;
  uint32_t swi_chunk;
  uint32_t offset_to_swi_handler;
  uint32_t offset_to_swi_decoding_table;
  uint32_t offset_to_swi_decoding_code;
  uint32_t offset_to_messages_file_name;
  uint32_t offset_to_flags;
};

typedef struct module module;

static inline bool is_queue( swi_action action )
{
  return (action.code & 3) == 1;
}

struct module {
  module_header *header;
  uint32_t private_word;
  module *next;                 // Simple singly-linked list
  module *instances;            // Simple singly-linked list of instances.
                                // Instance number is how far along the list 
                                // the module is, not a constant.
  module *base;                 // Base instance of this module
  swi_handlers *handlers;       // NULL for legacy modules
  char postfix[];
};

module *new_module( module_header *header, char const *postfix )
{
  int size = 0;
  while (postfix != 0 && postfix[size] != '\0') size++;

  module *base = 0;
  if (size != 0) {
    base = shared.module.modules;
    while (base != 0 && base->header != header) base = base->next;
    if (base == 0) PANIC; // Not sure what to do in this case
  }

  module *result = system_heap_allocate( sizeof( *header ) + size + 1 );
  if (result == (void*) 0xffffffff) PANIC;

  for (int i = 0; i < size; i++) result->postfix[i] = postfix[i];
  result->postfix[size] = '\0';

  result->header = header;
  result->private_word = 0;
  result->next = 0;
  result->instances = 0;
  result->handlers = 0;

  if (size == 0) { // No postfix
    result->base = 0;

    if (shared.module.last != 0) {
      shared.module.last->next = result;
    }
    shared.module.last = result;
    if (shared.module.modules == 0) {
      shared.module.modules = result;
    }
  }
  else {
    module **instance = &base->instances;
    while (*instance != 0) {
      instance = &(*instance)->next;
    }
    *instance = result;
    result->base = base;
  }

  return result;
}

static void *pointer_at_offset_from( void *base, uint32_t off )
{
  return (off == 0) ? 0 : ((uint8_t*) base) + off;
}

static inline void *start_code( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_start );
}

static inline void *init_code( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_initialisation );
}

static inline const char *help_string( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_help_string );
}

static inline const char *title_string( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_title_string );
}

// Case insensitive, nul, cr, lf, space, or % terminates.
// So "abc" matches "abc def", "abc%ghi", etc.
bool module_name_match( char const *left, char const *right )
{
  int diff = 0;

  while (diff == 0) {
    char l = *left++;
    char r = *right++;

    if ((l == 0 || l == 10 || l == 13 || l == ' ' || l == '%')
     && (r == 0 || r == 10 || r == 13 || r == ' ' || r == '%')) {
      return true;
    }

    diff = l - r;

    if (diff == 'a'-'A') {
      if (l >= 'a' && l <= 'z') diff = 0;
    }
    else if (diff == 'A'-'a') {
      if (r >= 'a' && r <= 'z') diff = 0;
    }
  }

  return false;
}

//static 
module_header *find_module_in_list( char const *name, uint32_t *list )
{
  module_header *header = 0;
  uint32_t *entry = list;
  while (*entry != 0) {
    header = (void*) (entry + 1);
    if (module_name_match( title_string( header ), name )) {
      return header;
    }
    entry += (*entry / sizeof( uint32_t ));
  }
  return 0;
}

module_header *find_module_in_rom( char const *name )
{
  for (int i = 0; rom_modules[i].name != 0; i++) {
    if (module_name_match( name, rom_modules[i].name )) {
      return rom_modules[i].start;
    }
  }
  return 0;
}

static inline uint32_t bcd( char c )
{
  if (c < '0' || c > '9') c = '0';
  return c - '0';
}

static void Service_ModulePostInit( module *module )
{
  // Service_ModulePreInit

  char const *postfix = module->postfix[0] == 0 ? 0 : module->postfix;
  char const *title = title_string( module->header );
  uint32_t bcd_version = 0;
  char const *help = help_string( module->header );
  while (*help != '\t' && *help != '\0') help++;
  while (*help == '\t') help++;
  while (*help == ' ' && *help != '\0') help++;
  while (*help == ' ' && *help != '\0') help++;
  while (*help != '\0' && *help != '.') {
    bcd_version = (bcd_version << 4) + bcd( *help++ );
  }
  if (*help == '.') {
    bcd_version = (bcd_version << 4) + bcd( help[1] );
    bcd_version = (bcd_version << 4) + bcd( help[2] );
  }
  else {
    bcd_version = (bcd_version << 8);
  }

  register module_header* header asm( "r0" ) = module->header;
  register uint32_t code asm( "r1" ) = 0xda;
  register char const *t asm( "r2" ) = title;
  register char const *p asm( "r3" ) = postfix;
  register uint32_t vers asm( "r4" ) = bcd_version;

  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ServiceCall | Xbit)
      , "r" (code), "r" (header), "r" (t), "r" (p), "r" (vers)
      : "lr", "cc" );
}

static inline 
error_block *run_initialisation_code( const char *env, module *m,
                                      uint32_t instance )
{
  // Modules may initialise other modules during their initialisation.
  module *old_in_init = shared.module.in_init;

  shared.module.in_init = m;

  uint32_t *code = init_code( m->header );

  if (code == 0) return 0;

  register uint32_t *non_kernel_code asm( "r14" ) = code;
  register uint32_t *private_word asm( "r12" ) = &m->private_word;
  register uint32_t _instance asm( "r11" ) = instance;
  register const char *environment asm( "r10" ) = env;

  error_block *error;

  asm volatile (
        "  blx r14"
      "\n  movvs %[error], r0"
      "\n  movvc %[error], #0"
      : [error] "=r" (error)
      : "r" (non_kernel_code)
      , "r" (private_word)
      , "r" (_instance)
      , "r" (environment)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );

  // No changes to the registers by the module are of any interest,
  // so avoid corrupting any by simply not storing them
  shared.module.in_init = old_in_init;

  if (error == 0) {
    Service_ModulePostInit( m );
  }

  return error;
}

static module *find_module_by_chunk( uint32_t swi )
{
  // TODO: have a lookup table for up to 8192 modules (bits 6-16, 18, 19)?
  // (Include a bit to indicate modernity?)

  // TODO: Optimise: store the found module in workspace, but check
  // the swi is in that chunk each time find_module_by_chunk is
  // called. (This can all be done internal to find_module_by_chunk.)

  uint32_t chunk = swi & ~0xff00003f;

  module *instance = shared.module.modules;
  while (instance != 0 && instance->header->swi_chunk != chunk) {
    instance = instance->next;
  }

  return instance;
}

bool needs_legacy_stack( uint32_t swi )
{
  if (swi < 0x200) return true; // kernel SWIs, currently all legacy

  module *m = find_module_by_chunk( swi );

  // We don't need a legacy stack to return unknown SWI, or if
  // the module has registered handlers.

  return (m != 0) && (m->handlers == 0);
}

OSTask *TaskOpRegisterSWIHandlers( svc_registers *regs )
{
  module *m = shared.module.in_init;
  if (m->handlers != 0) PANIC;

  swi_handlers const *handlers = (void*) regs->r[0];
  m->handlers = system_heap_allocate( sizeof( swi_handlers ) );
  if (m->handlers == (void*) 0xffffffff) PANIC;
  *m->handlers = *handlers;

  // Not, strictly speaking, a requirement, but having SWIs but not starting
  // at the first one is suspect.
  if (m->handlers->action[0].code == 0) PANIC;

  return 0;
}

void run_action( svc_registers *regs, uint32_t code, module *m )
{
  // CKernel SWI handlers take:
  //   r0-r9 from the caller
  //   r12   pointer to private word
  //   r11   undefined (no longer the SWI number; each one has its own code)
  //   r13   svc stack
  //   r14   return address
  //
  // May change r0-r9, flags, for the caller, corrupt r10-r12, r14.
  // New rule: condition flags are never inputs to (new) SWIs
  // (Honestly, I don't think there are SWIs that use them as inputs.)
  register uint32_t *private asm( "r12" ) = &m->private_word;
  register uint32_t c asm ( "r12" ) = code;
  register svc_registers *r asm ( "r11" ) = regs;
  asm volatile (
    "\n  push {r11}"
    "\n  ldmia r11, {r0-r9}"
    "\n  blx r12"
    "\n  pop {r11}"
    "\n  stmia r11, {r0-r9}"
    "\n  mrs r0, cpsr"
    "\n  str r0, [r11, %[psr]]"
    :
    : "r" (r)
    , "r" (c)
    , "r" (private)
    , [psr] "i" (offset_of( svc_registers, spsr ))
    : "lr", "cc", "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9"
  );
}

OSTask *run_module_swi( svc_registers *regs, int swi )
{
  module *m = find_module_by_chunk( swi );

  if (m == 0) {
    asm ( "udf 85" );
    return Error_UnknownSWI( regs );
  }

  if (m->handlers != 0) { // MP-aware
    uint32_t swi_offset = swi & 0x3f;
    swi_action action = m->handlers->action[swi_offset];
    if (is_queue( action ))
      return queue_running_OSTask( regs, action.queue, swi_offset );
    else if (action.code == 0) {
      asm ( "udf 86" );
      return Error_UnknownSWI( regs );
    }
    else {
      run_action( regs, action.code, m );
    }
  }
  else {
    PANIC; // Legacy module
  }

  return 0;
}

module_header *find_named_module( char const *name )
{
  module_header *header = 0;

  while (*name == ' ') name++;

  char const *rom_path = "System:Modules.";
  char const *mod_name = name;
  while (*mod_name == *rom_path) { mod_name++; rom_path++; }
  bool search_rom = (*rom_path == '\0');

  if (search_rom) {
    header = find_module_in_rom( mod_name );
    if (header == 0) {
      header = find_module_in_list( mod_name, &LegacyModulesList );
    }
  }
  else PANIC; // Look in filesystems

  if (header == 0) PANIC;

  return header;
}

module_header *load_named_module( char const *name )
{
  // Find a module with the given name; if it's on a filesystem,
  // load it into RMA, just after the file's length.

  module_header *header = find_named_module( name );

  if (header == 0) PANIC;

  // TODO: Load a module from a file system

  return header;
}

module *find_initialised_module( char const *name )
{
  module_header *header = find_named_module( name );

  module *instance = shared.module.modules;
  while (instance != 0 && instance->header != header) {
    instance = instance->next;
  }
  return instance;
}

error_block *load_and_initialise( char const *name )
{
  module_header *header = load_named_module( name );
  if (header == 0) {
    svc_registers tmp;
    Error_ModuleNotFound( &tmp );
    return (error_block *) tmp.r[0];
  }
  while (*name > ' ' && *name != '%') name++;
  module *m = new_module( header, (*name == '%') ? name + 1 : 0 );

  // Skip to start of any parameters
  while (*name > ' ') name++;
  while (*name == ' ') name++;

  uint32_t number = 0;
  if (0 != m->base) {
    module *instance = m->base->instances;
    while (instance != 0) {
      instance = instance->next;
      number++;
    }
  }

  return run_initialisation_code( name, m, number );
}

OSTask *do_OS_Module( svc_registers *regs )
{
  error_block *error = 0;
  char const *name = (void*) regs->r[1];

  switch (regs->r[0]) {
  case 0: // RMRun
    {
      module *m = find_initialised_module( name );
      if (m == 0) {
        error = load_and_initialise( name );
        if (error != 0) break;
        m = find_initialised_module( name );
        if (m == 0) PANIC;
      }
      void *start = start_code( m->header );
      if (start == 0) {
        static error_block err = { 0x888, "Module not startable" };
        error = &err;
        break;
      }
      // Not sure how long the command should exist for; until the
      // application is replaced, presumably. Put a copy in RMA and
      // associate it with the slot? TODO
      regs->r[0] = (uint32_t) name;
      regs->r[1] = (uint32_t) &m->private_word;
      regs->r[2] = (uint32_t) start;
    }
    break;
  case 1: // RMLoad
    error = load_and_initialise( name );
    break;
  case 6: // Claim
    regs->r[2] = (uint32_t) shared_heap_allocate( regs->r[3] );

    if (regs->r[2] == 0xffffffff) {
      return Error_NoRoomInRMA( regs );
    }
    break;
  case 7: // Free
    shared_heap_free( (void*) regs->r[2] );
    break;
  case 2: // RMEnter
  case 3: // RMReinit
  default:
    PANIC;
    break;
  }

  if (error != 0) {
    PANIC;
    regs->spsr |= VF;
    regs->r[0] = (uint32_t) error;
  }

  return 0;
}

