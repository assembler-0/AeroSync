/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/log.c
 * @brief Kernel logging subsystem
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <arch/x86_64/tsc.h>
#include <compiler.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>
#include <lib/log.h>
#include <lib/printk.h>
#include <lib/ringbuf.h>
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

#include <arch/x86_64/percpu.h>

// Per-CPU state for printk recursion and emergency buffers
DEFINE_PER_CPU(int, printk_recursion);
#define PRINTK_SAFE_BUF_SIZE 512
DEFINE_PER_CPU(char, printk_safe_buf[PRINTK_SAFE_BUF_SIZE]);

// Forward declaration for the klogd thread function used when creating kthreads
static int klogd_thread(void *data);

static uint8_t klog_ring_data[KLOG_RING_SIZE];
static ringbuf_t klog_ring = {
    .data = klog_ring_data, .size = KLOG_RING_SIZE, .head = 0, .tail = 0};

static int klog_console_level = KLOG_INFO;
static log_sink_putc_t klog_console_sink = NULL; // defaults to ring buffer only
static spinlock_t klog_lock = 0;
static int klog_inited = 1; // Statically initialized, so always ready

// Serialize immediate console output across CPUs to prevent mangled lines
static spinlock_t klog_console_lock = 0;
// Async logging control
static volatile int klog_async_enabled = 0;
static struct task_struct *klogd_task = NULL;
// Hint from the console driver that sink is asynchronous-capable
static int klog_console_sink_async_hint = 0;
// Debug enablement (independent of numeric KLOG_DEBUG value)
static int klog_debug_enabled = 0;
// Panic state: if non-zero, we ignore locks to ensure output
static volatile int panic_in_progress = 0;

static DECLARE_WAIT_QUEUE_HEAD(klogd_wait);

void log_mark_panic(void) { panic_in_progress = 1; }

// klogd drain budgeting to avoid monopolizing CPU on slow sinks (e.g.,
// linearfb)
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

static void drop_oldest(uint32_t need) {
  while (ringbuf_space(&klog_ring) < need) {
    klog_hdr_t hdr;
    if (ringbuf_peek(&klog_ring, &hdr, sizeof(hdr)) < sizeof(hdr))
      break;
    uint32_t drop = sizeof(hdr) + hdr.len;
    ringbuf_skip(&klog_ring, drop);
  }
}

void log_init(const log_sink_putc_t backend) {
  // We don't re-initialize the ringbuffer here if it already has data
  // ringbuf_init would reset head/tail.
  // If ringbuffer was somehow not setup (shouldn't happen with static init), do
  // it.
  if (klog_ring.data == NULL) {
    ringbuf_init(&klog_ring, klog_ring_data, KLOG_RING_SIZE);
  }

  klog_console_sink = backend;
}

void log_set_console_sink(log_sink_putc_t sink) {
  klog_console_sink = sink;
  /* If console sink has already indicated it is async-capable, try to
     start the background klogd so console emission can be deferred. */
  if (klog_console_sink_async_hint)
    log_try_init_async();
}

void log_set_console_level(int level) { klog_console_level = level; }
int log_get_console_level(void) { return klog_console_level; }

// Debug control helpers
void log_enable_debug(void) { klog_debug_enabled = 1; }
void log_disable_debug(void) { klog_debug_enabled = 0; }
int log_is_debug_enabled(void) { return klog_debug_enabled; }

// Console sink async hint
void log_set_console_async_hint(int is_async) {
  klog_console_sink_async_hint = is_async ? 1 : 0;
  /* If the sink is async-capable and a sink is already registered,
     attempt to bring up the background consumer. */
  if (klog_console_sink_async_hint && klog_console_sink)
    log_try_init_async();
}

// Try to initialize async klogd if scheduler is up. Returns non-zero on
// success.
int log_try_init_async(void) {
  if (klog_async_enabled)
    return 1;
  // If scheduler isn't available or kthread creation fails, return 0.
  struct task_struct *t = kthread_create(klogd_thread, NULL, "kthread/klogd");
  if (!t)
    return 0;
  kthread_run(t);
  klogd_task = t;
  klog_async_enabled = 1;
  return 1;
}

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
  if (tsc_freq_get() > 0) {
    // Check if calibrated
    uint64_t ns = ts_ns;
    uint64_t s = ns / 1000000000ULL;
    uint64_t us = (ns % 1000000000ULL) / 1000ULL;

    char ts_buf[48];
    snprintf(ts_buf, sizeof(ts_buf), "[%5llu.%06llu] ", s, us);

    char *t = ts_buf;
    while (*t)
      klog_console_sink(*t++);
  }
}

static int klog_sync_threshold = KLOG_ERR; // ERR and above stay synchronous

int log_write_str(int level, const char *msg) {
  if (!msg)
    return 0;

  // Recursion detection
  int rec = 0;
  if (percpu_ready()) {
    rec = this_cpu_read(printk_recursion);
    if (rec > 0) {
      // We are in a nested call (e.g. printk called from vsnprintf or sink)
      // We skip immediate console emission and just try to store in ring buffer
      // if possible, or if we are too deep, we drop to avoid infinite
      // recursion.
      if (rec > 3)
        return 0;
    }
    this_cpu_inc(printk_recursion);
  }

  // If level is DEBUG but debug is currently disabled, drop early
  if (level == KLOG_DEBUG && !klog_debug_enabled) {
    if (percpu_ready())
      this_cpu_dec(printk_recursion);
    return 0;
  }

  // Compute message length
  size_t len = 0;
  for (const char *p = msg; *p; ++p)
    len++;

  // Cap length to ring size - header - 1 to ensure progress
  size_t max_payload = (size_t)KLOG_RING_SIZE - sizeof(klog_hdr_t) - 1;
  if (len > max_payload)
    len = max_payload;

  // Producer timestamp captured once
  uint64_t ts_ns = get_time_ns();

  // Decide whether to emit synchronously.
  int do_sync_emit = 0;
  int effective_console_level = klog_console_level;
  if (klog_debug_enabled)
    effective_console_level = KLOG_DEBUG;

  if (klog_console_sink && level <= effective_console_level) {
    if (level <= effective_console_level)
      do_sync_emit = ((!klog_async_enabled && !klog_console_sink_async_hint) ||
                      (level <= klog_sync_threshold));
  }

  // Nested calls never emit synchronously to avoid deadlocking console_lock
  if (rec > 0)
    do_sync_emit = 0;

  uint8_t flags_hdr = 0;
  if (do_sync_emit) {
    irq_flags_t f;
    f = spinlock_lock_irqsave(&klog_console_lock);
    console_emit_prefix_ts(level, ts_ns);
    const char *p = msg;
    while (*p)
      klog_console_sink(*p++);
    spinlock_unlock_irqrestore(&klog_console_lock, f);
    flags_hdr |= KLOGF_SYNC_EMITTED;
  }

  // Always store in ring buffer regardless of sink presence
  irq_flags_t flags;
  flags = spinlock_lock_irqsave(&klog_lock);
  uint32_t need = (uint32_t)(sizeof(klog_hdr_t) + (uint32_t)len);
  if (ringbuf_space(&klog_ring) < need)
    drop_oldest(need);

  // Write header and payload
  klog_hdr_t hdr = {.level = (uint8_t)level,
                    .flags = flags_hdr,
                    .len = (uint16_t)len,
                    .ts_ns = ts_ns};
  ringbuf_write(&klog_ring, &hdr, sizeof(hdr));
  ringbuf_write(&klog_ring, msg, len);

  spinlock_unlock_irqrestore(&klog_lock, flags);

  if (percpu_ready())
    this_cpu_dec(printk_recursion);

  // Don't check_preempt here - IRQ state may be inconsistent immediately
  // after spinlock_unlock_irqrestore(). The scheduler will preempt naturally
  // on the next timer tick or syscall return.

  return (int)len;
}

// Background logger thread: drains ring buffer to console
// Background logger thread: drains ring buffer to console
static int klogd_thread(void *data) {
  (void)data;
  char out_buf[512];
  while (1) {
    int drained_any = 0;
    uint64_t slice_start = get_time_ns();
    int records = 0;
    size_t bytes = 0;
    for (;;) {
      int lvl = KLOG_INFO;
      uint64_t ts = 0;
      uint8_t flags_local = 0;
      // internal read to obtain ts/flags
      size_t n = 0;
      {
        // inline internal reader: duplicated logic of log_read to also fetch
        // ts/flags
        uint64_t lflags = spinlock_lock_irqsave(&klog_lock);
        if (!ringbuf_empty(&klog_ring)) {
          klog_hdr_t hdr;
          if (ringbuf_read(&klog_ring, &hdr, sizeof(hdr)) == sizeof(hdr)) {
            lvl = hdr.level;
            ts = hdr.ts_ns;
            flags_local = hdr.flags;
            size_t to_copy = hdr.len;
            if (to_copy > sizeof(out_buf) - 1)
              to_copy = sizeof(out_buf) - 1;
            n = ringbuf_read(&klog_ring, out_buf, to_copy);
            if (n < to_copy && hdr.len > to_copy) {
              ringbuf_skip(&klog_ring, hdr.len - to_copy);
            }
            out_buf[n] = '\0';
          }
        }
        spinlock_unlock_irqrestore(&klog_lock, lflags);
      }
      if (n == 0)
        break;

      // Recompute effective console level here as well so background
      // consumer honors the runtime debug enable flag.
      int effective_console_level_klogd = klog_console_level;
      if (klog_debug_enabled)
        effective_console_level_klogd = KLOG_DEBUG;

      if (!(flags_local & KLOGF_SYNC_EMITTED) && klog_console_sink &&
          lvl <= effective_console_level_klogd) {
        // In klogd context, avoid disabling IRQs while emitting to slow sinks
        // to reduce system-wide latency. Console lock still protects output
        // order.
        irq_flags_t cf = spinlock_lock_irqsave(&klog_console_lock);
        console_emit_prefix_ts(lvl, ts);
        for (size_t j = 0; j < n; ++j)
          klog_console_sink(out_buf[j]);
        spinlock_unlock_irqrestore(&klog_console_lock, cf);
      }
      drained_any = 1;
      records++;
      bytes += n;

      // Cooperative yield if we exceed any budget to avoid starving others
      uint64_t now = get_time_ns();
      if (records >= KLOGD_MAX_BATCH_RECORDS ||
          bytes >= (size_t)KLOGD_MAX_BATCH_BYTES ||
          (now - slice_start) >= KLOGD_MAX_SLICE_NS) {
        // Cooperative yield - only schedule once to prevent stack buildup.
        schedule();
        slice_start = get_time_ns();
        records = 0;
        bytes = 0;
      }
    }
    // No explicit schedule() needed here here, loop back to wait_event
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

  struct task_struct *t = kthread_create(klogd_thread, NULL, "kthread/klogd");
  if (t) {
    kthread_run(t);
    klogd_task = t;
    klog_async_enabled = 1;
  }
}

int log_read(char *out_buf, int out_buf_len, int *out_level) {
  if (!out_buf || out_buf_len <= 0)
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

  size_t actual = ringbuf_read(&klog_ring, out_buf, (size_t)to_copy);
  if (actual < (size_t)to_copy && hdr.len > (size_t)to_copy) {
    // Skip remaining data we couldn't fit
    ringbuf_skip(&klog_ring, hdr.len - (size_t)to_copy);
  }

  out_buf[actual] = '\0';
  if (out_level)
    *out_level = hdr.level;

  spinlock_unlock_irqrestore(&klog_lock, flags);
  return (int)actual;
}
