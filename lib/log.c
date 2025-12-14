#include <arch/x64/tsc.h>
#include <compiler.h>
#include <drivers/uart/serial.h>
#include <kernel/spinlock.h>
#include <lib/log.h>
#include <lib/ringbuf.h>
#include <lib/string.h>
#include <lib/vsprintf.h>

// Simple global ring buffer for log messages, Linux-like but minimal

#ifndef KLOG_RING_SIZE
#define KLOG_RING_SIZE (4096)
#endif

typedef struct {
  uint8_t level; // log level
  uint16_t len;  // payload length (bytes)
} __packed klog_hdr_t;

static char klog_ring_data[KLOG_RING_SIZE];
static ringbuf_t klog_ring;
static int klog_level = KLOG_INFO;
static int klog_console_level = KLOG_INFO;
static log_sink_putc_t klog_console_sink =
    serial_write_char; // default to serial
static spinlock_t klog_lock = 0;
static int klog_inited = 0;
// Serialize immediate console output across CPUs to prevent mangled lines
static spinlock_t klog_console_lock = 0;

static void rb_drop_oldest(uint32_t need) {
  while (ringbuf_space(&klog_ring) < need) {
    klog_hdr_t hdr;
    if (ringbuf_peek(&klog_ring, &hdr, sizeof(hdr)) < sizeof(hdr))
      break;
    uint32_t drop = sizeof(hdr) + hdr.len;
    ringbuf_skip(&klog_ring, drop);
  }
}

void log_init(const log_sink_putc_t backend) {
  ringbuf_init(&klog_ring, klog_ring_data, KLOG_RING_SIZE);
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

  // Add Timestamp [    0.000000]
  if (get_tsc_freq() > 0) { // Check if calibrated
    uint64_t ns = get_time_ns();
    uint64_t s = ns / 1000000000ULL;
    uint64_t us = (ns % 1000000000ULL) / 1000ULL;

    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "[%5llu.%06llu] ", s, us);
    // It's safe to use snprintf inside log system as long as it doesn't
    // recursively call printk and vsnprintf is safe.

    char *t = ts_buf;
    while (*t)
      klog_console_sink(*t++);
  }
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
  if (ringbuf_space(&klog_ring) < need)
    rb_drop_oldest(need);

  // Write header and payload
  klog_hdr_t hdr = {.level = (uint8_t)level, .len = (uint16_t)len};
  ringbuf_write(&klog_ring, &hdr, sizeof(hdr));
  ringbuf_write(&klog_ring, msg, len);

  spinlock_unlock_irqrestore(&klog_lock, flags);
  return len;
}

int log_read(char *out_buf, int out_buf_len, int *out_level) {
  if (!out_buf || out_buf_len <= 0)
    return 0;
  if (!klog_inited)
    return 0;

  uint64_t flags = spinlock_lock_irqsave(&klog_lock);

  if (ringbuf_empty(&klog_ring)) {
    spinlock_unlock_irqrestore(&klog_lock, flags);
    return 0; // empty
  }

  klog_hdr_t hdr;
  if (ringbuf_read(&klog_ring, &hdr, sizeof(hdr)) < sizeof(hdr)) {
    spinlock_unlock_irqrestore(&klog_lock, flags);
    return 0;
  }

  int to_copy = hdr.len;
  if (to_copy > out_buf_len - 1)
    to_copy = out_buf_len - 1; // reserve NUL

  int actual = ringbuf_read(&klog_ring, out_buf, to_copy);
  if (actual < to_copy && hdr.len > to_copy) {
    // Skip remaining data we couldn't fit
    ringbuf_skip(&klog_ring, hdr.len - to_copy);
  }

  out_buf[actual] = '\0';
  if (out_level)
    *out_level = hdr.level;

  spinlock_unlock_irqrestore(&klog_lock, flags);
  return actual;
}
