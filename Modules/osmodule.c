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

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

static inline void set_VF()
{
  asm ( "msr cpsr_f, #(1 << 28)" );
}

static int strlen( char const *s )
{
  char const *p = s;
  while ('\0' < *p) { p++; }
  return p - s;
}

static int rostrlen( char const *s )
{
  char const *p = s;
  while (' ' < *p || '\t' == *p) { p++; }
  return p - s;
}

typedef struct module_header module_header;

typedef struct {
  char const *name;
  module_header *start;
} rom_module;

#include "rom_modules.h"

#define NO_COMMAND 0x124
DEFINE_ERROR( ModuleNotFound, 0x888, "Module not found" );
DEFINE_ERROR( NoRoomInRMA, 0x888, "No room in RMA" );
DEFINE_ERROR( NoStart, 0x888, "Module not startable" );
DEFINE_ERROR( NoCommand, NO_COMMAND, "No module command found" );
DEFINE_ERROR( NoMoreModules, 0x108, "NoMoreModules:No more modules" );
DEFINE_ERROR( NoMoreInstances, 0x109, "NoMoreIncarnations:No more incarnations of that module" );
DECLARE_ERROR( UnknownSWI );

extern uint32_t const LegacyModulesList;

#define API_FLAG 8

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
  return (action.queue & 3) == 1;
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

static inline
void append_module_to_list( module *m )
{
  if (shared.module.last != 0) {
    shared.module.last->next = m;
  }
  shared.module.last = m;
  if (shared.module.modules == 0) {
    shared.module.modules = m;
  }
}

static void *pointer_at_offset_from( void *base, uint32_t off )
{
  return (off == 0) ? 0 : ((uint8_t*) base) + off;
}

static inline void *start_code( module_header const *header )
{
  return pointer_at_offset_from( header, header->offset_to_start );
}

static inline void *init_code( module_header const *header )
{
  return pointer_at_offset_from( header, header->offset_to_initialisation );
}

static inline const char *help_string( module_header const *header )
{
  return pointer_at_offset_from( header, header->offset_to_help_string );
}

static inline const char *title_string( module_header const *header )
{
  return pointer_at_offset_from( header, header->offset_to_title_string );
}

static inline void *service_call_handler( module_header const *h )
{
  return pointer_at_offset_from( h, h->offset_to_service_call_handler );
}

static inline void *module_commands( module_header const *h )
{
  return pointer_at_offset_from( h, h->offset_to_help_and_command_keyword_table );
}

static inline uint32_t module_flags( module_header const *h )
{
  return *(uint32_t*) pointer_at_offset_from( h, h->offset_to_flags );
}

static inline bool is_api_module( module_header const *h )
{
  return 0 != (4 & module_flags( h ));
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
module_header *find_module_in_list( char const *name, uint32_t const *list )
{
{ char const text[] = "Looking for module ";
Task_LogString( text, sizeof( text ) - 1 ); }
Task_LogString( name, rostrlen( name ) );
Task_LogString( "... ", 4 );

  module_header *header = 0;
  uint32_t const *entry = list;
  while (*entry != 0) {
    header = (void*) (entry + 1);

    if (module_name_match( title_string( header ), name )) {
{ char const text[] = "Match found ";
Task_LogString( text, sizeof( text ) - 1 ); }
Task_LogNewLine();
      return header;
    }
    entry += (*entry / sizeof( uint32_t ));
  }
{ char const text[] = "No matching module ";
Task_LogString( text, sizeof( text )-1 ); }
Task_LogNewLine();

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

static
int get_version( module_header const *m )
{
  uint32_t bcd_version = 0;
  char const *help = help_string( m );

  if (help != 0) {
    while (*help != '\t' && *help != '\0') help++;
    while (*help == '\t') help++;
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
  }

  return bcd_version;
}

static
void Service_ModulePostInit( module *module )
{
#ifdef DEBUG__SHOW_SERVICE_CALLS
  Task_LogString( "Service_ModulePostInit: ", 24 );
  Task_LogString( title_string( module->header ), 0 );
  Task_LogNewLine();
#endif

  char const *postfix = module->postfix[0] == 0 ? 0 : module->postfix;
  char const *title = title_string( module->header );
  uint32_t bcd_version = get_version( module->header );

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
error_block const *run_initialisation_code( const char *env, module *m,
                                      uint32_t instance )
{
  uint32_t *code = init_code( m->header );

  if (code == 0) {
    Task_LogString( "No initialisation code\n", 23 );
    append_module_to_list( m );
    assert( shared.module.modules != 0 );
    return 0;
  }

  // Modules may initialise other modules during their initialisation.
  module *old_in_init = shared.module.in_init;

  shared.module.in_init = m;

#ifdef DEBUG__SHOW_MODULES
  Task_LogString( "Init: ", 6 );
  Task_LogString( title_string( m->header ), 0 );
  if (*env > ' ') {
    Task_LogString( " ", 1 );
    Task_LogString( env, 0 );
  }
  else {
    Task_LogString( " no parameters", 14 );
  }
  Task_LogHex( (uint32_t) &m->private_word );
  Task_LogNewLine();
#endif

  register uint32_t *non_kernel_code asm( "r14" ) = code;
  register uint32_t *private_word asm( "r12" ) = &m->private_word;
  register uint32_t _instance asm( "r11" ) = instance;
  register const char *environment asm( "r10" ) = env;

  error_block const *error;

  asm volatile (
        "  blx r14"
      "\n  movvs %[error], r0"
      "\n  movvc %[error], #0"
      : [error] "=r" (error)
      : "r" (non_kernel_code)
      , "r" (private_word)
      , "r" (_instance)
      , "r" (environment)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "memory" );
      // Note, without "memory", the optimiser assumes shared.module.in_init
      // cannot be changed by the code and doesn't bother updating it.

  // No changes to the registers by the module are of any interest,
  // so avoid corrupting any by simply not storing them
  shared.module.in_init = old_in_init;

#ifdef DEBUG__SHOW_MODULES
  Task_LogString( "Returned from init", 0 );
  Task_LogNewLine();
#endif
  if (error == 0) {
    append_module_to_list( m );
    Service_ModulePostInit( m );
  }
  else {
    // TODO report error
// Don't PANIC;
  }

#ifdef DEBUG__SHOW_MODULES
  Task_LogString( "Done, pw ", 9 );
  Task_LogHex( (uint32_t) &m->private_word );
  Task_LogString( " ", 1 );
  if (error != 0)
    Task_LogHex( (uint32_t) error->code );
  Task_LogString( " ", 1 );
  Task_LogHex( (uint32_t) m->private_word );
  if (m->private_word == 0)
    Task_LogString( " empty!", 7 );
  Task_LogNewLine();
  asm ( "udf 2" );
#endif

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

// TODO: Cache these searches, you wouldn't be asking if you weren't
// about to call it!
bool handler_available( uint32_t swi )
{
  module *m = find_module_by_chunk( swi );

  return (m != 0) && (m->handlers != 0);
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

#ifdef DEBUG__SHOW_HANDLERS
  {
  char const text[] = "Handlers:";
  Task_LogString( text, sizeof( text )-1 );
  }
  for (int i = 0; i < 64; i++) {
    Task_LogString( " ", 1 );
    Task_LogHex( m->handlers->action[i].queue );
  }
  Task_LogNewLine();
#endif

  return 0;
}

uint32_t swi_handler( module_header *h )
{
  uint32_t result = h->offset_to_swi_handler;
  if (result != 0) result += (uint32_t) h;
  return result;
}

static void run_swi_handler_code( svc_registers *regs, uint32_t svc, module *m )
{
  register uint32_t non_kernel_code asm( "r14" ) = swi_handler( m->header );
  register uint32_t *private_word asm( "r12" ) = &m->private_word;
  register uint32_t svc_index asm( "r11" ) = svc & 0x3f;

  asm (
      "\n  push { %[regs] }"
      "\n  subs r0, r0, r0" // Clear V (also N, C, and set Z)
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r14"
      "\nreturn:"
      "\n  pop { %[regs] }"
      "\n  stm %[regs], { r0-r9 }"
      "\n  ldr r1, [%[regs], %[spsr]]"
      "\n  bic r1, #0xf0000000"
      "\n  mrs r2, cpsr"
      "\n  and r2, r2, #0xf0000000"
      "\n  orr r1, r1, r2"
      "\n  str r1, [%[regs], %[spsr]]"
      :
      : [regs] "r" (regs)
      , "r" (private_word)
      , "r" (svc_index)
      , "r" (non_kernel_code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );
}

OSTask *run_api_swi( svc_registers *regs, int swi )
{
  // find api modules by chunk
  // ...
  return 0;
}

OSTask *run_traditional_swi( svc_registers *regs, int swi )
{
  module *m = find_module_by_chunk( swi );

  if (m == 0) {
    asm ( "udf 85" );
    return Error_UnknownSWI( regs );
  }

  uint32_t swi_offset = swi & 0x3f;

  if (m->handlers != 0) { // MP-aware
    swi_action action = m->handlers->action[swi_offset];
    if (is_queue( action )) {
#ifdef DEBUG__FOLLOW_SWIS
    { char const text[] = "Queuing SWI ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( swi );
    { char const text[] = " for task ";
    Task_LogString( text, sizeof( text )-1 ); }
    Task_LogHex( ostask_handle( workspace.ostask.running ) );
    Task_LogNewLine();
#endif
      return queue_running_OSTask( regs, action.queue, swi_offset );
    }
    else if (action.code == 0) {
      asm ( "bkpt 86" );
      return Error_UnknownSWI( regs );
    }
    else {
      // PANIC; // Untested
      OSTask *running = workspace.ostask.running;
      action.code( regs, (void*) m->private_word,
                   workspace.core, ostask_handle( running ) );
      if (running != workspace.ostask.running) {
        // Task_QueueR12 was called
        PANIC; // Untested
        return workspace.ostask.running;
      }
    }
  }
  else {
    // Legacy module SWI, requires legacy stack ownership, can't check here 
    // because we don't know about it!
    run_swi_handler_code( regs, swi, m );
  }

  return 0;
}

static inline bool is_api_chunk( int swi )
{
  return false;
}

OSTask *run_module_swi( svc_registers *regs, int swi )
{
  if (is_api_chunk( swi )) {
    return run_api_swi( regs, swi );
  }
  else {
    return run_traditional_swi( regs, swi );
  }
}

char const *extract_module_name( char const *name )
{
  while (*name == ' ') name++;

  char const *rom_path = "System:Modules.";
  char const *mod_name = name;
  while (*mod_name == *rom_path) { mod_name++; rom_path++; }
  return ('\0' == *rom_path) ? mod_name : name;
}

module_header *find_named_module( char const *name )
{
  module_header *header = 0;

  header = find_module_in_rom( name );
  if (header == 0) {
    header = find_module_in_list( name, &LegacyModulesList );
  }

#ifdef DEBUG__SHOW_MODULE_INIT
  if (header == 0) {
    Task_LogString( "Module ", 7 );
    char const *e = name;
    while (*e > ' ') e++;
    Task_LogString( name, e - name );
    Task_LogString( " not found\n", 11 );
  }
#endif

  return header;
}

module_header *load_named_module( char const *name )
{
  // Find a module with the given name; if it's on a filesystem,
  // load it into RMA, just after the file's length.

  char const *const module_name = extract_module_name( name );

  module_header *header = find_named_module( module_name );

  if (header == 0) {
    // TODO: Load the module from a file system

  }

  return header;
}

module *find_initialised_module( char const *name )
{
  module_header *header = find_named_module( name );

  if (header == 0) return 0;

  module *instance = shared.module.modules;
  while (instance != 0 && instance->header != header) {
    instance = instance->next;
  }
  return instance;
}

error_block const *load_and_initialise( char const *name )
{
#ifdef DEBUG__SHOW_MODULE_INIT
  Task_LogString( "Module ", 7 );
  char const *e = name;
  while (*e > ' ') e++;
  Task_LogString( name, e - name );
  Task_LogString( " init\n", 6 );
#endif
  module_header *header = load_named_module( name );
  if (header == 0) {
    svc_registers tmp;
    Error_ModuleNotFound( &tmp );
    return (error_block const *) tmp.r[0];
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

  if (is_api_module( header )) {
    // This is where we register an API, if this is the first that
    // provides this chunk.
    // Either way, register the module as a provider.
    PANIC;
  }

  return run_initialisation_code( name, m, number );
}

static void __attribute__(( noinline )) run_service_call_handler_code( svc_registers *regs, module *m )
{
  register void *non_kernel_code asm( "r14" ) =
                        service_call_handler( m->header );

  register uint32_t *private_word asm( "r12" ) = &m->private_word;

  // "the V set convention cannot be used" PRM 1-217
  // Ignoring the input flags and not storing the output flags.

  asm (
        "  push { %[regs] }"
      "\n  ldm %[regs], { r0-r8 }"
      "\n  blx r14"
      "\n  pop { r14 }"
      "\n  stm r14, { r0-r8 }"
      :
      : [regs] "r" (regs)
      , [spsr] "i" (offset_of( svc_registers, spsr ))
      , "r" (non_kernel_code)
      , "r" (private_word)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8" );
}

OSTask *do_OS_ServiceCall( svc_registers *regs )
{
  module *m = shared.module.modules;
  if (m == 0) return 0;

  uint32_t call = regs->r[1];

#ifdef DEBUG__FOLLOW_SERVICE_CALLS
  Task_LogString( "Service call ", 13 );
  Task_LogSmallNumber( regs->r[1] );
  Task_LogNewLine();
#endif

  while (m != 0 && regs->r[1] != 0) {
    if (0 != m->header->offset_to_service_call_handler) {
      // Call for all instances, or just the base?
      if (m->instances != 0) PANIC;

#ifdef DEBUG__FOLLOW_SERVICE_CALLS
      Task_LogString( "  ", 2 );
      Task_LogString( title_string( m->header ), 0 );
#endif

      run_service_call_handler_code( regs, m );

      assert( regs->r[1] == 0 || regs->r[1] == call );
    }
#ifdef DEBUG__FOLLOW_SERVICE_CALLS
    if (regs->r[1] == 0) {
      Task_LogString( " - Claimed\n", 11 );
    }
#endif
    m = m->next;
  }

#ifdef DEBUG__FOLLOW_SERVICE_CALLS
  Task_LogNewLine();
#endif

  return 0;
}

extern OSTask *legacy_do_OS_Module( svc_registers *regs );

__attribute__(( weak ))
OSTask *legacy_do_OS_Module( svc_registers *regs )
{
  PANIC;
  return 0;
}

#ifdef DEBUG__SHOW_LEGACY_MODULES
void show_legacy_modules()
{
  uint32_t const *list = &LegacyModulesList;

  Task_LogString( "Legacy modules\n", 15 );
  int count = 0;

  module_header *header = 0;
  uint32_t const *entry = list;
  while (*entry != 0) {
    header = (void*) (entry + 1);

    Task_LogHex( *entry );
    Task_LogString( "  ", 2 );
    Task_LogString( title_string( header ), 0 );
    Task_LogNewLine();

    entry += (*entry / sizeof( uint32_t ));
    count++;
  }

  Task_LogString( "END\n", 4 );
  if (count < 10) PANIC;
}
#endif

// The legacy kernel no longer has control of the initialised modules
// list, so commands provided by modules have to be run from here, but
// I think I can leave the handling of executable files to the legacy
// kernel by passing on the vector call.
// There will be a small performance hit from de-aliasing commands in
// two places.
// This will also handle spawning commands using the Spawn command,
// alias & (TODO)

// This vector will only be called owning the legacy stack....
// Illicit knowledge?
// At least we know that the value of the Alias variable will not change
// before we call some external code.

typedef enum { HANDLER_PASS_ON, HANDLER_INTERCEPTED, HANDLER_FAILED } handled;

typedef struct {
  char const *command;
  char const *tail;
} command_parts;

static command_parts split_command( char const *cmd )
{
  command_parts result;

  result.command = cmd;
  while (*result.command == ' '
     ||  *result.command == '*') result.command++;
  // result.command points to the first non-space, non-asterisk character
  // That may be a '%'.

  result.tail = result.command;
  while (*result.tail == '%') result.tail++;
  while (*result.tail == ' ') result.tail++;
  while (*result.tail > ' ') result.tail++;
  while (*result.tail == ' ') result.tail++;
  // result.tail points to the first non-space character after the command

  return result;
}

// Case insensitive, nul, cr, lf, or space terminate
static inline bool riscoscmp( char const *left, char const *right )
{
  int diff = 0;

#ifdef DEBUG__riscoscmp
  Task_LogString( left, 0 );
  Task_LogString( " ", 1 );
  Task_LogString( right, 0 );
#endif
  while (diff == 0) {
    char l = *left++;
    char r = *right++;

    if ((l == 0 || l == 10 || l == 13 || l == ' ')
     && (r == 0 || r == 10 || r == 13 || r == ' ')) {
#ifdef DEBUG__riscoscmp
  Task_LogString( " match\n", 7 );
#endif
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

#ifdef DEBUG__riscoscmp
  Task_LogString( " no match\n", 10 );
#endif
  return false;
}

typedef struct __attribute__(( packed, aligned( 4 ) )) {
  uint32_t code_offset;
  struct {
    uint8_t min_params;
    uint8_t gstrans;
    uint8_t max_params;
    uint8_t flags;
  } info;
  uint32_t invalid_syntax_offset;
  uint32_t help_offset;
} module_command;

typedef struct {
  module *module;
  module_command *command;
} module_code;

static module_command *search_list( char const *command, const char *cmd )
{
  if (cmd != 0) {
    while (cmd[0] != '\0') {
      int len = strlen( cmd );

      module_command *c = (void*) (((uint32_t) cmd + len + 4)&~3);
      // +4 because len is strlen, not including terminator

      if (riscoscmp( cmd, command ) && c->code_offset != 0) {
        return c;
      }

      cmd = (char const *) (c + 1);
    }
  }

  return 0;
}

static module_code find_module_command( char const *command )
{
  module *m = shared.module.modules;

  while (m != 0) {
    module_header *header = m->header;

    const char *cmd = module_commands( header );

    module_command *found = search_list( command, cmd );

    if (found != 0) {
      module_code result = { m, found };
      return result;
    }

    m = m->next;
  }

  module_code no_match = { 0, 0 };
  return no_match;
}

static inline bool terminator( char c )
{
  return c == '\0' || c == '\r' || c == '\n';
}

static uint32_t count_params( char const *params )
{
  uint32_t result = 0;
  char const *p = params;

  while (*p == ' ') p++;

  while (!terminator( *p )) {

    result ++;

    while (!terminator( *p ) && *p != ' ') {
      if ('"' == *p) {
        do {
          p ++;
        } while (!terminator( *p ) && *p != '"');
        if (terminator( *p )) return -1; // Mistake
        // Otherwise p points to closing quote...
      }
      p++;
    }

    while (*p == ' ') p++;
  }

#ifdef DEBUG__SHOW_COUNT_PARAMS
  Task_LogString( "Tail: \"", 7 );
  Task_LogString( params, 0 );
  Task_LogString( "\" ", 2 );
  Task_LogSmallNumber( result );
  Task_LogNewLine();
#endif

  return result;
}

//static 
__attribute__(( noinline ))
error_block const *run_command( module *m, uint32_t code_offset, const char *tail, uint32_t count )
{
  Task_LogHex( code_offset + (uint32_t) m->header );

  register uint32_t non_kernel_code asm( "r14" ) = code_offset + (uint32_t) m->header;
  // Bodge for funny commands
  register uint32_t private_word asm( "r12" ) = m == 0 ? 0 : &m->private_word;
  register const char *p asm( "r0" ) = tail;
  register uint32_t c asm( "r1" ) = count;

  register error_block const *error asm( "r0" );

  asm volatile (
      "\n  blx r14"
      "\n  movvc r0, #0"
      : "=r" (error)
      : "r" (p)
      , "r" (c)
      , "r" (non_kernel_code)
      , "r" (private_word)
      : "r2", "r3", "r4", "r5", "r6" );

  return error;
}

static error_block const *run_module_code( module_code code, char const *tail )
{
  while (*tail == ' ') tail++;
  uint32_t count = count_params( tail );
  module_command const *c = code.command;
  module *m = code.module;

#ifdef DEBUG__SHOW_COUNT_PARAMS
  Task_LogString( "Expecting between ", 18 );
  Task_LogSmallNumber( c->info.min_params );
  Task_LogString( " and ", 5 );
  Task_LogSmallNumber( c->info.max_params );
  Task_LogNewLine();
#endif

  if (count < c->info.min_params || count > c->info.max_params) {
    asm ( "udf 77" );
    static error_block error = { 666, "Invalid number of parameters" };
    // TODO Service_SyntaxError
    return &error;
  }
  else if (count == -1) {
    static error_block mistake = { 4, "Mistake" };
    return &mistake;
  }

  if (c->info.gstrans != 0 && count > 0) {
    // Need to copy the command, running GSTrans on some parameters
    Task_LogString( "The code in run_module_code needs expanding, not GSTransing parameters\n", 0 );
    Task_LogString( tail, rostrlen( tail ) );
    Task_LogNewLine();
/*
    static error_block needs_more_code = { 4, "The code in run_module_code needs expanding" };
    return &needs_more_code;
    asm ( "bkpt 1" );
*/
  }

  return run_command( m, c->code_offset, tail, count );
}

// FIXME: Deal with Set Alias$A B ; Set Alias$B A?
__attribute__(( noinline ))
handled run_aliased_command( uint32_t *regs )
{
  command_parts parts = split_command( (void*) regs[0] );
  char const *cmd = parts.command;
  char const *tail = parts.tail;

  // %Command is dealt with part way through, once the command has been
  // copied.

  char const Alias[] = "Alias$";
  int varsize = sizeof( Alias ); // Includes terminator
  char const *name = cmd;
  while (*name > ' ') {
    varsize++;
    // If it's a file name, forget it, leave it to the legacy kernel
    if (*name == ':' || *name == '.') return false;
    name++;
  }
  char varname[varsize];
  char const *s = Alias;
  char *d = varname;
  while (*s > ' ') *d++ = *s++;
  char const *cmd0 = d;
  s = cmd;
  while (*s > ' ') *d++ = *s++;
  *d = '\0';

  // varname now contains "Alias$" + the command, possibly including
  // % characters. If the command starts with a %, the Alias$ part will
  // be ignored.

  uint32_t space = 0;

  if (cmd[0] != '%') {
    register char const *name asm ( "r0" ) = varname;
    // r1 ignored because r2 -ve
    register int32_t asksize asm ( "r2" ) = -1;
    register uint32_t context asm ( "r3" ) = 0;
    register uint32_t convert asm ( "r4" ) = 0; // No conversion
    register int32_t size asm ( "r2" );
    register uint32_t type asm ( "r4" );
    asm ( "svc %[swi]"
        : "=r" (size)
        , "=r" (type)
        , "=r" (context)
        : [swi] "i" (OS_ReadVarVal)
        , "r" (name)
        , "r" (asksize)
        , "r" (context)
        , "r" (convert)
        : "lr" );

    if (size != 0) {
      space = !size;
    }
  }
  else {
    // Skip preceding % characters and subsequent spaces
    while (*cmd0 == '%') cmd0++;
    while (*cmd0 == ' ') cmd0++;
  }

  module_code code = { 0, 0 };

  if (space != 0) {
    char alias[space+1];

    register char const *name asm ( "r0" ) = varname;
    register char const *buffer asm ( "r1" ) = alias;
    register int32_t asksize asm ( "r2" ) = space;
    register uint32_t context asm ( "r3" ) = 0;
    register uint32_t convert asm ( "r4" ) = 0; // No conversion
    register int32_t size asm ( "r2" );
    register uint32_t type asm ( "r4" );
    asm ( "svc %[swi]"
        : "=r" (size)
        , "=r" (type)
        , "=r" (context)
        : [swi] "i" (OS_ReadVarVal)
        , "r" (name)
        , "r" (buffer)
        , "r" (asksize)
        , "r" (context)
        , "r" (convert)
        : "lr", "memory" );

    if (size != space) PANIC;

    alias[space] = '\0'; // ReadVarVal doesn't terminate output

    code = find_module_command( alias );
  }
  else {
    // Not aliased.
    code = find_module_command( cmd0 );
  }

  if (code.command == 0) {
#ifdef DEBUG__LOG_COMMANDS
    Task_LogString( "Command ", 8 );
    Task_LogString( cmd0, 0 );
    Task_LogString( " not found in modules\n", 22 );
#endif
    clear_VF();
    return HANDLER_PASS_ON;
  }

#ifdef DEBUG__LOG_COMMANDS
    Task_LogString( "Running ", 8 );
    Task_LogString( cmd0, 0 );
    Task_LogString( " module: ", 9 );
    Task_LogHex( code.module->header );
    Task_LogString( " code: ", 7 );
    Task_LogHex( code.command );
    Task_LogString( " offset: ", 9 );
    Task_LogHex( code.command->code_offset );
    Task_LogNewLine();
#endif
  error_block const *error = run_module_code( code, tail );

  if (error != 0) {
    regs[0] = (uint32_t) error;
#ifdef DEBUG__LOG_COMMANDS
    Task_LogString( "Error from ", 11 );
    Task_LogString( cmd0, 0 );
    Task_LogString( ": ", 2 );
    Task_LogString( error->desc, 0 );
    Task_LogNewLine();
#endif
    return HANDLER_FAILED;
  }

  return HANDLER_INTERCEPTED;
}


__attribute__(( naked ))
void run_module_command()
{
  // The code will be called in SVC mode.
  uint32_t *regs;

  asm ( "push { r0-r9, r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  asm ( "push {lr}" ); // Normal return address, to continue down the list

  handled result = run_aliased_command( regs );
  switch (result) {
  case HANDLER_FAILED: // Intercepted, but failed
  case HANDLER_INTERCEPTED:
    if (result == HANDLER_FAILED)
      set_VF();
    else
      clear_VF();
    asm ( "pop {lr}\n  pop { r0-r9, r12, pc }" );
    break;
  case HANDLER_PASS_ON:
    asm ( "pop {lr}\n  pop { r0-r9, r12 }\n  mov pc, lr" );
    break;
  default: PANIC;
  }
}

void claim_CLIV()
{
#ifdef DEBUG__SHOW_LEGACY_MODULES
  show_legacy_modules();
#endif
  register uint32_t vector asm( "r0" ) = 5;
  register void *routine asm( "r1" ) = run_module_command;
  //register struct workspace *handler_workspace asm( "r2" ) = ws;
  // Always provide the same "workspace", so repeated claims
  // replace the previous.
  register uint32_t handler_workspace asm( "r2" ) = 0x83838383;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_Claim | Xbit)
      , "r" (vector)
      , "r" (routine)
      , "r" (handler_workspace)
      : "lr" );
}

static inline __attribute__(( noreturn ))
void start_usr32( void *start, uint32_t *private )
{
  // Not sure how long the command should exist for; until the
  // application is replaced, presumably. Put a copy in RMA and
  // associate it with the slot? TODO
  // Set arguments as for OS_FSControl 2? TODO
  // Start address in r4, private word in r3, "command line" in r2

  register uint32_t r0 asm( "r0" ) = 2;
  register uint32_t r1 asm( "r1" ) = 0;
  register char const *r2 asm( "r2" ) = "SomeCommand";
  register uint32_t *r3 asm( "r3" ) = private;
  register void *r4 asm( "r4" ) = start;
  register uint32_t r12 asm( "r12" ) = 0x5943474c; // "LGCY"
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_Finished)
      , "r" (r0)
      , "r" (r1)
      , "r" (r2)
      , "r" (r3)
      , "r" (r4)
      , "r" (r12)
      );

  __builtin_unreachable();
}

OSTask *do_OS_Module( svc_registers *regs )
{
  // Hint to the caller that the stack should be reset, etc.
  // Only for RMRun and RMEnter
  static error_block no_error = { 0, "No error - enter module" };

  error_block const *error = 0;
  char const *const name = (void*) regs->r[1];

#ifdef LEGACY_CLIV
  if (shared.module.modules == 0) {
    // Even if the load_and_initialise doesn't modify 
    // shared.module.modules, a subsequent claim will simply
    // replace the previous.
    claim_CLIV();
  }
#endif

  switch (regs->r[0]) {
  case 0: // RMRun
    {
      char const *const module_name = extract_module_name( name );
      module *m = find_initialised_module( module_name );
      if (m == 0) {
        error = load_and_initialise( name );
        if (error != 0) break;
        m = find_initialised_module( module_name );
        if (m == 0) PANIC; // Any error already reported
      }
      void *start = start_code( m->header );
      if (start == 0) {
        return Error_NoStart( regs );
      }
#ifdef DEBUG__SHOW_MODULE_INIT
  Task_LogString( "RMRun module ", 13 );
  char const *e = name;
  while (*e > ' ') e++;
  Task_LogString( name, e - name );
  Task_LogNewLine();
#endif

      // Treat this as a call to return the start and private addresses
      regs->r[0] = (uint32_t) &no_error;
      regs->spsr |= VF;
      regs->r[1] = (uint32_t) start;
      regs->r[2] = (uint32_t) &m->private_word;
    }
    break;
  case 1: // RMLoad
#ifdef DEBUG__CHECK_INTERRUPT_HANDLING
  if (extract_module_name( name )[0] == '&') {
    asm ( "cpsie i" );
    for (;;) {
      // V flag should never change in the inner loops
      asm ( "msr cpsr_f, #0x10000000" );
      asm ( "0:"
        "\n  sub %[i], %[i], #1"
        "\n  tst %[i], #27"
        "\n  bne 0b"
        :
        : [i] "r" (0xffff0000) );
      asm ( "msr cpsr_f, #0xe0000000" );
      asm ( "0:"
        "\n  sub %[i], %[i], #1"
        "\n  tst %[i], #27"
        "\n  bne 0b"
        :
        : [i] "r" (0xffff0000) );
    }
  }
#endif
    error = load_and_initialise( name );
    break;
  case 2: // RMEnter
    {
      char const *const module_name = extract_module_name( name );
      module *m = find_initialised_module( module_name );
      if (m == 0) PANIC;

      void *start = start_code( m->header );
      if (start == 0) PANIC;

      // Treat this as a call to return the start and private addresses
      regs->r[0] = (uint32_t) &no_error;
      regs->spsr |= VF;
      regs->r[1] = start;
      regs->r[2] = &m->private_word;
    }
    break;
  case 12: // Extract module information
    // Called from legacy RMEnsure
    {
      uint32_t number = regs->r[1];
      uint32_t inst = regs->r[2];

      module *m = shared.module.modules;
      uint32_t n = 0;
      while (m != 0 && n < number) {
        m = m->next;
        n++;
      }

      regs->r[1]++;
      if (m == 0) {
        return Error_NoMoreModules( regs );
      }
      else {
        regs->r[3] = (uint32_t) m->header;
        regs->r[4] = (uint32_t) m->private_word;
        regs->r[5] = (uint32_t) &m->postfix;
      }
    }
    break;
  case 18: // Look-up module name (not really what it does)
    // Called from legacy RMEnsure
    {
      char const *name = (void*) regs->r[1];

#ifdef DEBUG__SHOW_MODULE_INIT
  Task_LogString( "Lookup module ", 14 );
  char const *e = name;
  while (*e > ' ') e++;
  Task_LogString( name, e - name );
  Task_LogNewLine();
#endif

      //  TODO: instances
      char const *p = name;
      while (*p != '\0' && *p != '%' && *p != ' ') p++;
      if (*p == '%') PANIC;

      module *m = find_initialised_module( name );
      if (m == 0) {
#ifdef DEBUG__SHOW_MODULE_INIT
  Task_LogString( "Not found\n", 10 );
#endif
        return Error_ModuleNotFound( regs );
      }
      else {
#ifdef DEBUG__SHOW_MODULE_INIT
  Task_LogString( "Module found\n", 13 );
#endif
        uint32_t n = 0;
        module *p = shared.module.modules;
        while (p != m) { n++; p = p->next; }
        regs->r[1] = n;
        regs->r[2] = 0;
        regs->r[3] = (uint32_t) m->header; // FIXME: Guessed
        regs->r[4] = m->private_word;
        if (regs->r[2] != 0)
          regs->r[5] = (uint32_t) &m->postfix;
      }
    }
    break;
  case 3: // RMReinit
  case 4: // Delete
    PANIC;
    break;
  default:
    return legacy_do_OS_Module( regs );
  }

  if (error != 0) {
#ifdef DEBUG__SHOW_MODULE_INIT
    Task_LogString( "Module init ERROR: ", 19 );
    Task_LogString( error->desc, 0 );
    Task_LogNewLine();
#endif
    regs->spsr |= VF;
    regs->r[0] = (uint32_t) error;
  }

  return 0;
}

