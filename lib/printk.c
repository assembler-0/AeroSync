/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/printk.c
 * @brief printk backend management and logging functions
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

#include <arch/x86_64/io.h>
#include <lib/log.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/timer.h>
#include <aerosync/types.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/string.h>
#include <aerosync/fkx/fkx.h>

#define MAX_PRINTK_BACKENDS 8

static const printk_backend_t *registered_backends[MAX_PRINTK_BACKENDS];
static int num_registered_backends = 0;
static const printk_backend_t *active_backend = nullptr;
static const printk_backend_t *last_active_backend = nullptr;
static bool printk_disabled = false;

void printk_register_backend(const printk_backend_t *backend) {
  if (!backend || num_registered_backends >= MAX_PRINTK_BACKENDS) {
    return;
  }
  registered_backends[num_registered_backends++] = backend;
}
EXPORT_SYMBOL(printk_register_backend);

void __no_cfi printk_auto_configure(void *payload, const int reinit) {
  const printk_backend_t *best = nullptr;

  for (int i = 0; i < num_registered_backends; i++) {
    const printk_backend_t *b = registered_backends[i];
    if (!b || !b->probe)
      continue;

    if (!b->probe())
      continue;

    if (b->init && b->init(payload) != 0)
      continue;

    if (!best || b->priority > best->priority)
      best = b;
  }

  if (!best) {
    // Fallback to internal ringbuffer only
    if (reinit) log_set_console_sink(nullptr);
    else log_init(nullptr);
    
    // We can still printk, it will go to ringbuffer
    active_backend = nullptr;
    printk(KERN_ERR KERN_CLASS "no active printk backend, logging to ringbuffer only\n");
    return;
  }

  if (active_backend != best) {
    active_backend = best;
    last_active_backend = best;
  }

  if (!printk_disabled) {
    if (reinit) {
      log_set_console_sink(best->putc);
    } else {
      log_init(best->putc);
    }
    
    printk(KERN_CLASS "printk backend selected: %s (prio=%d)\n",
            best->name, best->priority);
  } else {
    if (reinit) {
      log_set_console_sink(nullptr);
    } else {
      log_init(nullptr);
    }
  }
}

#ifdef ASYNC_PRINTK
void printk_init_async(void) {
  printk(KERN_CLASS "starting asynchronous printk.\n");
  log_init_async();
}
#endif

int __no_cfi printk_set_sink(const char *backend_name, bool cleanup) {
  if (!backend_name) {
    printk_shutdown();
    return 0;
  }

  for (int i = 0; i < num_registered_backends; i++) {
    const printk_backend_t *b = registered_backends[i];
    if (b && b->name && strcmp(b->name, backend_name) == 0) {
      if (active_backend && active_backend->cleanup && cleanup) {
        active_backend->cleanup();
      }

      if (b->init) {
        int needs_init = 1;
        if (b->is_active) needs_init = !b->is_active();
        
        if (needs_init && b->init(nullptr) != 0) {
          printk(KERN_ERR KERN_CLASS "failed to reinit printk backend %s\n", backend_name);
          // Fallback to auto select if preferred backend failed
          const printk_backend_t *fallback = printk_auto_select_backend(backend_name);
          if (fallback) return printk_set_sink(fallback->name, false);
          return -ENODEV;
        }
      }

      active_backend = b;
      last_active_backend = b;
      
      if (!printk_disabled) {
        log_set_console_sink(b->putc);
      }
      return 0;
    }
  }
  
  // Backend not found, try fallback
  printk(KERN_ERR KERN_CLASS "printk backend %s not found, falling back\n", backend_name);
  const printk_backend_t *fallback = printk_auto_select_backend(backend_name);
  if (fallback) return printk_set_sink(fallback->name, false);

  return -ENODEV;
}
EXPORT_SYMBOL(printk_set_sink);

void printk_disable(void) {
  if (printk_disabled) return;
  
  printk_disabled = true;
  last_active_backend = active_backend;
  active_backend = nullptr;
  log_set_console_sink(nullptr);
}
EXPORT_SYMBOL(printk_disable);

void printk_enable(void) {
  if (!printk_disabled) return;
  
  printk_disabled = false;
  if (last_active_backend) {
    if (printk_set_sink(last_active_backend->name, false) == 0) {
      return;
    }
  }
  
  // Restore failed or none saved, re-configure
  printk_auto_configure(nullptr, 1);
}
EXPORT_SYMBOL(printk_enable);

const printk_backend_t *printk_auto_select_backend(const char *not) {
  const printk_backend_t *best = nullptr;
  for (int i = 0; i < num_registered_backends; i++) {
    const printk_backend_t *b = registered_backends[i];
    if (!b) continue;

    if ((not && b->name && strcmp(b->name, not) == 0) || b == active_backend) {
      continue;
    }
    if (!best || b->priority > best->priority)
      best = b;
  }
  return best;
}
EXPORT_SYMBOL(printk_auto_select_backend);

void __no_cfi printk_shutdown(void) {
  if (active_backend && active_backend->cleanup) {
    active_backend->cleanup();
  }
  active_backend = nullptr;
  last_active_backend = nullptr;
  log_set_console_sink(nullptr);
}
EXPORT_SYMBOL(printk_shutdown);

static const char *parse_level_prefix(const char *fmt, int *level_io) {
  // Safety check for null or too-short strings
  if (!fmt[0] || !fmt[1] || !fmt[2])
    return fmt;

  // format: $<0-7>$ (see include/lib/printk.h for level definitions)
  if (fmt[0] == '$' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '$') {
    if (level_io)
      *level_io = (fmt[1] - '0');
    return fmt + 3; // Skip past the level prefix
  }

  return fmt; // No level prefix found, return original
}

int vprintk(const char *fmt, va_list args) {
  if (!fmt)
    return -EINVAL;

  int level = KLOG_INFO; // Default log level
  // Parse optional level prefix (e.g. "$3$")
  const char *real_fmt = parse_level_prefix(fmt, &level);

  char local_buf[256];
  int count = vsnprintf(local_buf, sizeof(local_buf), real_fmt, args);

  // Write to log subsystem
  log_write_str(level, local_buf);

  return count;
}
EXPORT_SYMBOL(vprintk);

int vprintkln(const char *fmt, va_list args) {
  if (!fmt)
    return -EINVAL;

  int level = KLOG_INFO; // Default log level
  // Parse optional level prefix (e.g. "$3$")
  const char *real_fmt = parse_level_prefix(fmt, &level);

  char local_buf[256];
  int count = vsnprintf(local_buf, sizeof(local_buf), real_fmt, args);
  strcat(local_buf, "\n");

  // Write to log subsystem
  log_write_str(level, local_buf);

  return count;
}
EXPORT_SYMBOL(vprintkln);

int printk(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vprintk(fmt, args);
  va_end(args);
  return ret;
}
EXPORT_SYMBOL(printk);

int printkln(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vprintkln(fmt, args);
  va_end(args);
  return ret;
}
EXPORT_SYMBOL(printkln);

int ___ratelimit(ratelimit_state_t *rs, const char *func) {
  if (!rs) return 1;

  irq_flags_t flags = spinlock_lock_irqsave(&rs->lock);
  uint64_t now = get_time_ns();
  uint64_t interval_ns = (uint64_t)rs->interval * 1000000ULL;

  if (!rs->begin || (now - rs->begin) >= interval_ns) {
    if (rs->missed > 0) {
      // Use raw printk to avoid recursive ratelimit check
      printk(KERN_WARNING KERN_CLASS "%d messages suppressed by %s\n", rs->missed, func);
    }
    rs->begin = now;
    rs->printed = 0;
    rs->missed = 0;
  }

  if (rs->printed < rs->burst) {
    rs->printed++;
    spinlock_unlock_irqrestore(&rs->lock, flags);
    return 1;
  }

  rs->missed++;
  spinlock_unlock_irqrestore(&rs->lock, flags);
  return 0;
}
EXPORT_SYMBOL(___ratelimit);