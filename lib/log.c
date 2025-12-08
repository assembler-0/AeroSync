#include <compiler.h>
#include <drivers/uart/serial.h>
#include <kernel/spinlock.h>
#include <lib/log.h>
#include <lib/string.h>

// Simple global ring buffer for log messages, Linux-like but minimal

#ifndef KLOG_RING_SIZE
#define KLOG_RING_SIZE (4096)
#endif

typedef struct {
  uint8_t level; // log level
  uint16_t len;  // payload length (bytes)
} __packed klog_hdr_t;

static char klog_ring[KLOG_RING_SIZE];
static uint32_t klog_head; // write position
static uint32_t klog_tail; // read position
static int klog_level = KLOG_INFO;
static int klog_console_level = KLOG_INFO;
static log_sink_putc_t klog_console_sink = serial_write_char; // default to serial
static spinlock_t klog_lock = 0;
static int klog_inited = 0;
// Serialize immediate console output across CPUs to prevent mangled lines
static spinlock_t klog_console_lock = 0;

static inline uint32_t rb_space(void) {
  // one byte left unused rule to distinguish full/empty
  if (klog_head >= klog_tail)
    return (KLOG_RING_SIZE - (klog_head - klog_tail) - 1);
  return (klog_tail - klog_head - 1);
}

static void rb_advance(uint32_t *p, uint32_t n) {
  *p = (*p + n) % KLOG_RING_SIZE;
}

static void rb_drop_oldest(uint32_t need) {
  // Drop oldest records until 'need' bytes available
  while (rb_space() < need) {
    // Read header at tail
    klog_hdr_t hdr;
    if (KLOG_RING_SIZE - klog_tail >= sizeof(hdr)) {
      memcpy(&hdr, &klog_ring[klog_tail], sizeof(hdr));
    } else {
      // header wraps
      char tmp[sizeof(hdr)];
      uint32_t first = KLOG_RING_SIZE - klog_tail;
      memcpy(tmp, &klog_ring[klog_tail], first);
      memcpy(tmp + first, &klog_ring[0], sizeof(hdr) - first);
      memcpy(&hdr, tmp, sizeof(hdr));
    }
    uint32_t drop = sizeof(hdr) + hdr.len;
    rb_advance(&klog_tail, drop);
  }
}

void log_init(const log_sink_putc_t backend) {
  klog_head = klog_tail = 0;
  klog_level = KLOG_INFO;
  klog_console_level = KLOG_INFO;
  klog_console_sink = backend;
  klog_inited = 1;
}

void log_set_level(int level) { klog_level = level; }
int log_get_level(void) { return klog_level; }

void log_set_console_sink(log_sink_putc_t sink) { klog_console_sink = sink; }
void log_set_console_level(int level) { klog_console_level = level; }
int log_get_console_level(void) { return klog_console_level; }

static const char *const klog_prefixes[] = {
    [KLOG_EMERG] = "[0] ", [KLOG_ALERT] = "[1] ",   [KLOG_CRIT] = "[2] ",
    [KLOG_ERR] = "[3] ",   [KLOG_WARNING] = "[4] ", [KLOG_NOTICE] = "[5] ",
    [KLOG_INFO] = "[6] ",  [KLOG_DEBUG] = "[7] ",
};

static void console_emit_prefix(int level) {
  if (!klog_console_sink)
    return;

  const char *pfx = (level >= 0 && level < 8) ? klog_prefixes[level]
                                              : klog_prefixes[KLOG_INFO];

  while (*pfx)
    klog_console_sink(*pfx++);
}

int log_write_str(int level, const char *msg) {
  if (!msg)
    return 0;

  // Console output (immediate) respecting console level
  if (klog_console_sink && level <= klog_console_level) {
    irq_flags_t f = spinlock_lock_irqsave(&klog_console_lock);
    console_emit_prefix(level);
    const char *p = msg;
    while (*p)
      klog_console_sink(*p++);
    spinlock_unlock_irqrestore(&klog_console_lock, f);
  }

  if (!klog_inited)
    return 0; // before init, console-only

  if (level > klog_level) {
    // Level too low — do not store
    return 0;
  }

  // Compute message length
  int len = 0;
  for (const char *p = msg; *p; ++p)
    len++;

  // Cap length to ring size - header - 1 to ensure progress
  int max_payload = (int)KLOG_RING_SIZE - (int)sizeof(klog_hdr_t) - 1;
  if (len > max_payload)
    len = max_payload;

  uint64_t flags = spinlock_lock_irqsave(&klog_lock);

  uint32_t need = sizeof(klog_hdr_t) + (uint32_t)len;
  if (rb_space() < need)
    rb_drop_oldest(need);

  // Write header (possibly wrapping)
  klog_hdr_t hdr = {.level = (uint8_t)level, .len = (uint16_t)len};
  if (KLOG_RING_SIZE - klog_head >= sizeof(hdr)) {
    memcpy(&klog_ring[klog_head], &hdr, sizeof(hdr));
    rb_advance(&klog_head, sizeof(hdr));
  } else {
    uint32_t first = KLOG_RING_SIZE - klog_head;
    memcpy(&klog_ring[klog_head], &hdr, first);
    memcpy(&klog_ring[0], ((char *)&hdr) + first, sizeof(hdr) - first);
    rb_advance(&klog_head, sizeof(hdr));
  }

  // Write payload - use volatile to prevent over-aggressive optimization
  volatile char *ring_ptr = (volatile char *)&klog_ring[klog_head];
  volatile const char *msg_ptr = (volatile const char *)msg;

  if (KLOG_RING_SIZE - klog_head >= (uint32_t)len) {
    for (int i = 0; i < len; i++) {
      ring_ptr[i] = msg_ptr[i];
    }
    rb_advance(&klog_head, (uint32_t)len);
  } else {
    uint32_t first = KLOG_RING_SIZE - klog_head;
    for (uint32_t i = 0; i < first; i++) {
      ring_ptr[i] = msg_ptr[i];
    }
    volatile char *ring_start = (volatile char *)&klog_ring[0];
    for (uint32_t i = 0; i < (uint32_t)len - first; i++) {
      ring_start[i] = msg_ptr[first + i];
    }
    rb_advance(&klog_head, (uint32_t)len);
  }

  spinlock_unlock_irqrestore(&klog_lock, flags);
  return len;
}

int log_read(char *out_buf, int out_buf_len, int *out_level) {
  if (!out_buf || out_buf_len <= 0)
    return 0;
  if (!klog_inited)
    return 0;

  uint64_t flags = spinlock_lock_irqsave(&klog_lock);

  if (klog_head == klog_tail) {
    spinlock_unlock_irqrestore(&klog_lock, flags);
    return 0; // empty
  }

  klog_hdr_t hdr;
  if (KLOG_RING_SIZE - klog_tail >= sizeof(hdr)) {
    memcpy(&hdr, &klog_ring[klog_tail], sizeof(hdr));
    rb_advance(&klog_tail, sizeof(hdr));
  } else {
    char tmp[sizeof(hdr)];
    uint32_t first = KLOG_RING_SIZE - klog_tail;
    memcpy(tmp, &klog_ring[klog_tail], first);
    memcpy(tmp + first, &klog_ring[0], sizeof(hdr) - first);
    memcpy(&hdr, tmp, sizeof(hdr));
    rb_advance(&klog_tail, sizeof(hdr));
  }

  int to_copy = hdr.len;
  if (to_copy > out_buf_len - 1)
    to_copy = out_buf_len - 1; // reserve NUL

  if (KLOG_RING_SIZE - klog_tail >= (uint32_t)to_copy) {
    memcpy(out_buf, &klog_ring[klog_tail], (uint32_t)to_copy);
    rb_advance(&klog_tail, hdr.len);
  } else {
    uint32_t first = KLOG_RING_SIZE - klog_tail;
    if (first > (uint32_t)to_copy)
      first = (uint32_t)to_copy;
    memcpy(out_buf, &klog_ring[klog_tail], first);
    if ((uint32_t)to_copy > first) {
      memcpy(out_buf + first, &klog_ring[0], (uint32_t)to_copy - first);
    }
    rb_advance(&klog_tail, hdr.len);
  }

  out_buf[to_copy] = '\0';
  if (out_level)
    *out_level = hdr.level;

  spinlock_unlock_irqrestore(&klog_lock, flags);
  return to_copy;
}
