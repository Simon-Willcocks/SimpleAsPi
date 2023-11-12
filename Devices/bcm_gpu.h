typedef struct {
  uint32_t value; // Request or Response, depending if from or to ARM,
                  // (Pointer & 0xfffffff0) | Channel 0-15
  uint32_t res1;
  uint32_t res2;
  uint32_t res3;
  uint32_t peek;  // Doesn't remove the value from the FIFO
  uint32_t sender;// ??
  uint32_t status;// bit 31: Tx full, 30: Rx empty
  uint32_t config;
} GPU_mailbox;

typedef struct __attribute__(( packed )) {
  uint32_t to0x200[0x200/4];
  // 0x200
  union {
    struct {
      uint32_t basic_pending;
      uint32_t pending1;
      uint32_t pending2;
      uint32_t fiq_control;
      uint32_t enable_irqs1;
      uint32_t enable_irqs2;
      uint32_t enable_basic;
      uint32_t disable_irqs1;
      uint32_t disable_irqs2;
      uint32_t disable_basic;
    };
    uint32_t to0x400[0x200/4];
  };
  // 0x400
  union {
    struct {
      uint32_t load;
      uint32_t value;
      uint32_t control;
      uint32_t irq;
      uint32_t irq_raw;
      uint32_t irq_masked;
      uint32_t pre_deivider;
      uint32_t counter;
    } regular_timer;
    uint32_t to0x880[0x480/4];
  };
  // 0x880
  GPU_mailbox mailbox[2]; // ARM may read mailbox 0, write mailbox 1.

} GPU;

struct workspace {
