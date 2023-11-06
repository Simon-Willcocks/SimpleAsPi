
void _exit( int );

#define PANIC _exit( 1 )

uint32_t change_word_if_equal( uint32_t *lock, uint32_t c, uint32_t n )
{
  uint32_t v = *lock;
  if (v == c) *lock = n;
  return v;
}
static inline void ensure_changes_observable() { }

static inline void push_writes_to_cache() { }

static inline void signal_event() { }

static inline void wait_for_event() { }

