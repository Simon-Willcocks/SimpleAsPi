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

extern uint32_t CKernelModulesList;
extern uint32_t LegacyModulesList;

typedef struct {
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
} module_header;

typedef struct module module;

static bool is_queue( uint32_t code )
{
  return (code & 3) == 1;
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

static void pre_init_service( module_header *module, uint32_t size )
{
  // FIXME TODO

  return;
#if 0
  // Service_ModulePreInit
  register module_header* header asm( "r0" ) = module;
  register uint32_t code asm( "r1" ) = 0xb9;
  register uint32_t length_plus_four asm( "r2" ) = size;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ServiceCall | Xbit)
      , "r" (code), "r" (header), "r" (length_plus_four)
#endif
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

  return error;
}


bool needs_legacy_stack( uint32_t swi )
{
  return true;
}

OSTask *TaskOpRegisterSWIHandlers( svc_registers *regs )
{
  module *m = shared.module.in_init;
  if (m->handlers != 0) PANIC;

  swi_handlers const *handlers = (void*) regs->r[0];
  m->handlers = system_heap_allocate( sizeof( swi_handlers ) );
  if (m->handlers == (void*) 0xffffffff) PANIC;
  *m->handlers = *handlers;

  return 0;
}

OSTask *run_module_swi( svc_registers *regs, int swi )
{
  return 0;
}

module_header *find_named_module( char const *name )
{
  module_header *header = 0;

  while (*name == ' ') name++;

  char const *rom_module = "System:Modules.";
  char const *mod_name = name;
  while (*mod_name == *rom_module) { mod_name++; rom_module++; }
  bool search_rom = (*rom_module == '\0');

  if (search_rom) {
    header = find_module_in_list( mod_name, &CKernelModulesList );
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

DEFINE_ERROR( ModuleNotFound, 0x888, "Module not found" );
DEFINE_ERROR( NoRoomInRMA, 0x888, "No room in RMA" );

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
      regs->r[0] = (uint32_t) "There should be a command line here!";
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

