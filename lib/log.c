#include <arch/x64/tsc.h>
#include <compiler.h>
#include <drivers/uart/serial.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <kernel/spinlock.h>
#include <lib/log.h>
#include <lib/ringbuf.h>
#include <lib/string.h>
#include <lib/vsprintf.h>

// Simple global ring buffer for log messages, Linux-like but minimal

#ifndef KLOG_RING_SIZE
#define KLOG_RING_SIZE (4096)
#endif

// Flags for klog header
#define KLOGF_SYNC_EMITTED 0x01 // already emitted to console synchronously

typedef struct {
  uint8_t level;  // log level
  uint8_t flags;  // see KLOGF_*
  uint16_t len;   // payload length (bytes)
  uint64_t ts_ns; // producer timestamp in nanoseconds
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
// Async logging control
static volatile int klog_async_enabled = 0;
static struct task_struct *klogd_task = NULL;

// klogd drain budgeting to avoid monopolizing CPU on slow sinks (e.g., linearfb)
#ifndef KLOGD_MAX_BATCH_RECORDS
#define KLOGD_MAX_BATCH_RECORDS 64
#endif
#ifndef KLOGD_MAX_BATCH_BYTES
#define KLOGD_MAX_BATCH_BYTES 4096
#endif
#ifndef KLOGD_MAX_SLICE_NS
// Time budget per active drain slice (~2 ms)
#define KLOGD_MAX_SLICE_NS (2ULL * 1000ULL * 1000ULL)
#endif

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

static void console_emit_prefix_ts(int level, uint64_t ts_ns) {
  if (!klog_console_sink)
    return;

  const char *pfx = (level >= 0 && level < 8) ? klog_prefixes[level]
                                              : klog_prefixes[KLOG_INFO];

  while (*pfx)
    klog_console_sink(*pfx++);

  // Add Timestamp [    0.000000]
  if (get_tsc_freq() > 0) { // Check if calibrated
    uint64_t ns = ts_ns;
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

static void console_emit_prefix(int level) { console_emit_prefix_ts(level, get_time_ns()); }

static int klog_sync_threshold = KLOG_ERR; // ERR and above stay synchronous

int log_write_str(int level, const char *msg) {
  if (!msg)
    return 0;

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

  // Producer timestamp captured once, used for both sync and async output
  uint64_t ts_ns = get_time_ns();

  // Console output (immediate) respecting console level
  // If async is enabled, only emit synchronously for high-severity
  int do_sync_emit = 0;
  if (klog_console_sink && level <= klog_console_level) {
    if (!klog_async_enabled) {
      do_sync_emit = 1;
    } else if (level <= klog_sync_threshold) {
      do_sync_emit = 1;
    }
  }

  uint8_t flags_hdr = 0;
  if (do_sync_emit) {
    irq_flags_t f = spinlock_lock_irqsave(&klog_console_lock);
    console_emit_prefix_ts(level, ts_ns);
    const char *p = msg;
    while (*p)
      klog_console_sink(*p++);
    spinlock_unlock_irqrestore(&klog_console_lock, f);
    flags_hdr |= KLOGF_SYNC_EMITTED;
  }

  uint64_t flags = spinlock_lock_irqsave(&klog_lock);

  uint32_t need = sizeof(klog_hdr_t) + (uint32_t)len;
  if (ringbuf_space(&klog_ring) < need)
    rb_drop_oldest(need);

  // Write header and payload
  klog_hdr_t hdr = {.level = (uint8_t)level, .flags = flags_hdr, .len = (uint16_t)len, .ts_ns = ts_ns};
  ringbuf_write(&klog_ring, &hdr, sizeof(hdr));
  ringbuf_write(&klog_ring, msg, len);

  spinlock_unlock_irqrestore(&klog_lock, flags);
  
  // Hint scheduler to run klogd sooner in process context
  if (klog_async_enabled)
    check_preempt();
  return len;
}

// Background logger thread: drains ring buffer to console
static int klogd_thread(void *data) {
  (void)data;
  char out_buf[512];
  while (1) {
    int drained_any = 0;
    uint64_t slice_start = get_time_ns();
    int records = 0;
    int bytes = 0;
    for (;;) {
      int lvl = KLOG_INFO;
      uint64_t ts = 0;
      uint8_t flags_local = 0;
      // internal read to obtain ts/flags
      int n = 0;
      {
        // inline internal reader: duplicated logic of log_read to also fetch ts/flags
        uint64_t lflags = spinlock_lock_irqsave(&klog_lock);
        if (!ringbuf_empty(&klog_ring)) {
          klog_hdr_t hdr;
          if (ringbuf_read(&klog_ring, &hdr, sizeof(hdr)) == (int)sizeof(hdr)) {
            lvl = hdr.level;
            ts = hdr.ts_ns;
            flags_local = hdr.flags;
            int to_copy = hdr.len;
            if (to_copy > (int)sizeof(out_buf) - 1)
              to_copy = (int)sizeof(out_buf) - 1;
            n = ringbuf_read(&klog_ring, out_buf, to_copy);
            if (n < to_copy && hdr.len > to_copy) {
              ringbuf_skip(&klog_ring, hdr.len - to_copy);
            }
            out_buf[n] = '\0';
          }
        }
        spinlock_unlock_irqrestore(&klog_lock, lflags);
      }
      if (n <= 0)
        break;

      if (!(flags_local & KLOGF_SYNC_EMITTED) && klog_console_sink && lvl <= klog_console_level) {
        // In klogd context, avoid disabling IRQs while emitting to slow sinks
        // to reduce system-wide latency. Console lock still protects output order.
        spinlock_lock(&klog_console_lock);
        console_emit_prefix_ts(lvl, ts);
        for (int j = 0; j < n; ++j)
          klog_console_sink(out_buf[j]);
        spinlock_unlock(&klog_console_lock);
      }
      drained_any = 1;
      records++;
      bytes += n;

      // Cooperative yield if we exceed any budget to avoid starving others
      uint64_t now = get_time_ns();
      if (records >= KLOGD_MAX_BATCH_RECORDS || bytes >= KLOGD_MAX_BATCH_BYTES ||
          (now - slice_start) >= KLOGD_MAX_SLICE_NS) {
        check_preempt();
        schedule();
        slice_start = get_time_ns();
        records = 0;
        bytes = 0;
      }
    }
    if (!drained_any) {
      // Nothing to do, yield CPU
      schedule();
    }
  }
}

void log_init_async(void) {
  if (klog_async_enabled)
    return;
  // Create a low-priority kernel thread to drain the ring buffer
  // Drop any pre-existing buffered records to avoid duplicate console output
  {
    uint64_t f = spinlock_lock_irqsave(&klog_lock);
    ringbuf_init(&klog_ring, klog_ring_data, KLOG_RING_SIZE);
    spinlock_unlock_irqrestore(&klog_lock, f);
  }

  struct task_struct *t = kthread_create(klogd_thread, NULL, 5, "kthread/klogd");
  if (t) {
    kthread_run(t);
    klogd_task = t;
    klog_async_enabled = 1;
  }
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