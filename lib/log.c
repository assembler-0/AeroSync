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
#include <aerosync/compiler.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/export.h>
#include <lib/log.h>
#include <linux/kfifo.h>
#include <lib/string.h>

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

static DEFINE_KFIFO(klog_fifo, uint8_t, KLOG_RING_SIZE);

static int klog_console_level = KLOG_INFO;
static log_sink_putc_t klog_console_sink = nullptr; // defaults to ring buffer only
static DEFINE_SPINLOCK(klog_lock);

// Serialize immediate console output across CPUs to prevent mangled lines
static DEFINE_SPINLOCK(klog_console_lock);
// Async logging control
static volatile int klog_async_enabled = 0;
static struct task_struct *klogd_task = nullptr;
// Hint from the console driver that sink is asynchronous-capable
static int klog_console_sink_async_hint = 0;
// Debug enablement (independent of numeric KLOG_DEBUG value)
static int klog_debug_enabled = 0;
// Panic state: if non-zero, we ignore locks to ensure output
static volatile int panic_in_progress = 0;

void log_mark_panic(void) { panic_in_progress = 1; }
EXPORT_SYMBOL(log_mark_panic);

static void drop_oldest(uint32_t need) {
  while (kfifo_avail(&klog_fifo) < need) {
    klog_hdr_t hdr;
    if (kfifo_out_peek(&klog_fifo, (void *)&hdr, sizeof(hdr)) < sizeof(hdr))
      break;
    uint32_t drop = sizeof(hdr) + hdr.len;
    kfifo_skip_count(&klog_fifo, drop);
  }
}

void log_init(const log_sink_putc_t backend) {
  // Static initialization handles the klog_fifo.
  klog_console_sink = backend;
}

void log_set_console_sink(log_sink_putc_t sink) {
  klog_console_sink = sink;
  /* If console sink has already indicated it is async-capable, try to
     start the background klogd so console emission can be deferred. */
#ifdef ASYNC_PRINTK
  if (klog_console_sink_async_hint)
    log_try_init_async();
#endif
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
#ifdef ASYNC_PRINTK
  if (klog_console_sink_async_hint && klog_console_sink)
    log_try_init_async();
#endif
}

#ifdef ASYNC_PRINTK
// Try to initialize async klogd if scheduler is up. Returns non-zero on
// success.
int log_try_init_async(void) {
  if (klog_async_enabled)
    return 1;
  // If scheduler isn't available or kthread creation fails, return 0.
  struct task_struct *t = kthread_create(klogd_thread, nullptr, "kthread/klogd");
  if (!t)
    return 0;
  kthread_run(t);
  klogd_task = t;
  klog_async_enabled = 1;
  return 1;
}
#endif

static const char *const klog_prefixes[] = {
  [KLOG_EMERG] = "[0] ",
  [KLOG_ALERT] = "[1] ",
  [KLOG_CRIT] = "[2] ",
  [KLOG_ERR] = "[3] ",
  [KLOG_WARNING] = "[4] ",
  [KLOG_NOTICE] = "[5] ",
  [KLOG_INFO] = "[6] ",
  [KLOG_DEBUG] = "[7] ",
};

static void __no_cfi console_emit_prefix_ts(int level, uint64_t ts_ns) {
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

/**
 * log_flush_fifo_locked - Drain the FIFO to the console sink
 * MUST be called with klog_console_lock held.
 */
static void log_flush_fifo_locked(void) {
  if (!klog_console_sink)
    return;

  char out_buf[512];
  int effective_console_level = klog_console_level;
  if (klog_debug_enabled)
    effective_console_level = KLOG_DEBUG;

  while (!kfifo_is_empty(&klog_fifo)) {
    klog_hdr_t hdr;
    irq_flags_t flags = spinlock_lock_irqsave(&klog_lock);
    
    // Peek at header
    if (kfifo_out_peek(&klog_fifo, (void *)&hdr, sizeof(hdr)) < sizeof(hdr)) {
      spinlock_unlock_irqrestore(&klog_lock, flags);
      break;
    }

    if (hdr.flags & KLOGF_SYNC_EMITTED) {
      // Already printed, just skip it in the FIFO
      kfifo_skip_count(&klog_fifo, sizeof(hdr) + hdr.len);
      spinlock_unlock_irqrestore(&klog_lock, flags);
      continue;
    }

    // Read the record fully
    kfifo_skip_count(&klog_fifo, sizeof(hdr));
    size_t to_read = hdr.len;
    if (to_read > sizeof(out_buf) - 1)
      to_read = sizeof(out_buf) - 1;
    
    unsigned int n = kfifo_out(&klog_fifo, (void *)out_buf, (unsigned int)to_read);
    if (n < hdr.len) {
      kfifo_skip_count(&klog_fifo, hdr.len - n);
    }
    spinlock_unlock_irqrestore(&klog_lock, flags);

    if (hdr.level <= effective_console_level || hdr.level == KLOG_RAW) {
      if (hdr.level != KLOG_RAW) console_emit_prefix_ts(hdr.level, hdr.ts_ns);
      for (unsigned int i = 0; i < n; i++)
        klog_console_sink(out_buf[i]);
    }
  }
}

static int klog_sync_threshold = KLOG_ERR; // ERR and above stay synchronous

static int early_printk_recursion = 0;

int __no_cfi log_write_str(int level, const char *msg) {
  if (!msg)
    return 0;

  // Recursion detection
  int rec = 0;
  if (percpu_ready()) {
    rec = this_cpu_read(printk_recursion);
    if (rec > 0) {
      if (rec > 3)
        return 0;
    }
    this_cpu_inc(printk_recursion);
  } else {
    rec = early_printk_recursion;
    if (rec > 0) {
      if (rec > 3)
        return 0;
    }
    early_printk_recursion++;
  }

  // If level is DEBUG but debug is currently disabled, drop early
  if (level == KLOG_DEBUG && !klog_debug_enabled) {
    if (percpu_ready())
      this_cpu_dec(printk_recursion);
    else
      early_printk_recursion--;
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
    if (level != KLOG_RAW) console_emit_prefix_ts(level, ts_ns);
    const char *p = msg;
    while (*p)
      klog_console_sink(*p++);
    spinlock_unlock_irqrestore(&klog_console_lock, f);
    flags_hdr |= KLOGF_SYNC_EMITTED;
  }

  // Always store in kfifo regardless of sink presence
  irq_flags_t flags;
  flags = spinlock_lock_irqsave(&klog_lock);
  uint32_t need = (uint32_t)(sizeof(klog_hdr_t) + (uint32_t)len);
  if (kfifo_avail(&klog_fifo) < need)
    drop_oldest(need);

  // Write header and payload
  klog_hdr_t hdr = {.level = (uint8_t)level,
                    .flags = flags_hdr,
                    .len = (uint16_t)len,
                    .ts_ns = ts_ns};
  kfifo_in(&klog_fifo, (void *)&hdr, sizeof(hdr));
  kfifo_in(&klog_fifo, (void *)msg, (unsigned int)len);

  spinlock_unlock_irqrestore(&klog_lock, flags);

  if (percpu_ready())
    this_cpu_dec(printk_recursion);
  else
    early_printk_recursion--;

  return (int)len;
}

// Background logger thread: drains FIFO to console
static int __no_cfi klogd_thread(void *data) {
  (void)data;
  while (1) {
    if (!kfifo_is_empty(&klog_fifo)) {
      irq_flags_t f = spinlock_lock_irqsave(&klog_console_lock);
      log_flush_fifo_locked();
      spinlock_unlock_irqrestore(&klog_console_lock, f);
    }
    // Cooperative yield
    schedule();
  }
  return 0;
}

int log_init_async(void) {
  if (klog_async_enabled)
    return -EALREADY;
  // Create a low-priority kernel thread to drain the FIFO
  {
    uint64_t f = spinlock_lock_irqsave(&klog_lock);
    kfifo_reset(&klog_fifo);
    spinlock_unlock_irqrestore(&klog_lock, f);
  }

  struct task_struct *t = kthread_create(klogd_thread, nullptr, "kthread/klogd");
  if (t) {
    kthread_run(t);
    klogd_task = t;
    klog_async_enabled = 1;
  } else return -ENOMEM;
  return 0;
}

int log_read(char *out_buf, int out_buf_len, int *out_level) {
  if (!out_buf || out_buf_len <= 0)
    return 0;

  uint64_t flags = spinlock_lock_irqsave(&klog_lock);

  if (kfifo_is_empty(&klog_fifo)) {
    spinlock_unlock_irqrestore(&klog_lock, flags);
    return 0; // empty
  }

  klog_hdr_t hdr;
  if (kfifo_out_peek(&klog_fifo, (void *)&hdr, sizeof(hdr)) < sizeof(hdr)) {
    spinlock_unlock_irqrestore(&klog_lock, flags);
    return 0;
  }

  kfifo_skip_count(&klog_fifo, sizeof(hdr));

  int to_copy = hdr.len;
  if (to_copy > out_buf_len - 1)
    to_copy = out_buf_len - 1; // reserve NUL

  size_t actual = kfifo_out(&klog_fifo, (void *)out_buf, (unsigned int)to_copy);
  if (actual < (size_t)to_copy && hdr.len > (size_t)to_copy) {
    // Skip remaining data we couldn't fit
    kfifo_skip_count(&klog_fifo, hdr.len - (size_t)to_copy);
  }

  out_buf[actual] = '\0';
  if (out_level)
    *out_level = hdr.level;

  spinlock_unlock_irqrestore(&klog_lock, flags);
  return (int)actual;
}
