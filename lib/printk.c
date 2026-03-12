/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/printk.c
 * @brief printk backend management and logging functions
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/timer.h>
#include <aerosync/types.h>
#include <arch/x86_64/io.h>
#include <lib/log.h>
#include <lib/printk.h>
#include <lib/string.h>

#include "arch/x86_64/requests.h"

#define MAX_PRINTK_BACKENDS 8

static printk_backend_t *registered_backends[MAX_PRINTK_BACKENDS];
static int num_registered_backends = 0;
static const printk_backend_t *active_backend = nullptr;
static const printk_backend_t *manual_backend = nullptr;
static bool printk_disabled = false;

/* --- Internal Helpers --- */

static void sort_backends(void) {
  for (int i = 0; i < num_registered_backends - 1; i++) {
    for (int j = 0; j < num_registered_backends - i - 1; j++) {
      if (registered_backends[j]->priority < registered_backends[j + 1]->priority) {
        printk_backend_t *tmp = registered_backends[j];
        registered_backends[j] = registered_backends[j + 1];
        registered_backends[j + 1] = tmp;
      }
    }
  }
}

static int backend_try_init(printk_backend_t *b, void *payload) {
  if (!b) return -EINVAL;

  /* Skip if not ready */
  if (b->state == PRINTK_BACKEND_PENDING) {
    return -EAGAIN;
  }

  if (b->probe && !b->probe()) {
    return -ENODEV;
  }

  if (b->init) {
    /* If backend reports it is active, we might skip init or re-init?
     * Logic: if is_active() returns true, hardware is ready.
     * But we might still need software init. Standardize on calling init.
     */
    if (b->init(payload) != 0) {
      return -EIO;
    }
  }

  return 0;
}

static void backend_set_active(const printk_backend_t *b, int reinit) {
  if (active_backend == b) return;

  if (active_backend && active_backend->cleanup) {
    active_backend->cleanup();
  }

  active_backend = b;

  if (!printk_disabled) {
    if (reinit) {
      log_set_console_sink(b->putc, b->write);
    } else {
      log_init(b->putc, b->write);
    }
    printk(KERN_NOTICE KERN_CLASS "printk: switched to '%s' (prio=%d)\n",
           b->name, b->priority);
  } else {
    /* If disabled, we still update active_backend but don't set sink yet */
    if (reinit) log_set_console_sink(nullptr, nullptr);
    else log_init(nullptr, nullptr);
  }
}

/* --- Public API --- */

void printk_register_backend(printk_backend_t *backend) {
  if (!backend || num_registered_backends >= MAX_PRINTK_BACKENDS) {
    return;
  }

  /* Check for duplicates */
  for (int i = 0; i < num_registered_backends; i++) {
    if (registered_backends[i] == backend ||
        (backend->name && strcmp(registered_backends[i]->name, backend->name) == 0)) {
      return;
    }
  }

  /* Traditional registration assumes READY if not already PENDING */
  if (backend->state == PRINTK_BACKEND_DEFAULT) {
    backend->state = PRINTK_BACKEND_READY;
  }

  registered_backends[num_registered_backends++] = backend;
  sort_backends();

  /* If no manual override, auto-switch if this is the new best */
  if (!manual_backend && !printk_disabled && backend->state == PRINTK_BACKEND_READY
      && !cmdline_find_option_bool(current_cmdline, "printk_no_auto_switch")
  ) {
    printk_auto_configure(nullptr, 1);
  }
}

EXPORT_SYMBOL(printk_register_backend);

void printk_register_backend_pending(printk_backend_t *backend) {
  if (!backend) return;
  backend->state = PRINTK_BACKEND_PENDING;
  printk_register_backend(backend);
}

EXPORT_SYMBOL(printk_register_backend_pending);

void printk_backend_signal_ready(printk_backend_t *backend) {
  if (!backend) return;
  backend->state = PRINTK_BACKEND_READY;

  /* Re-evaluate backends now that one is ready */
  if (!manual_backend && !printk_disabled
      && !cmdline_find_option_bool(current_cmdline, "printk_no_auto_switch")
  ) {
    printk_auto_configure(nullptr, 1);
  }
}

EXPORT_SYMBOL(printk_backend_signal_ready);

int printk_unregister_backend(const printk_backend_t *backend, const int force) {
  if (!backend) return -EINVAL;
  if (backend == active_backend && !force) return -EBUSY;

  int idx = -1;
  for (int i = 0; i < num_registered_backends; i++) {
    if (registered_backends[i] == backend) {
      idx = i;
      break;
    }
  }

  if (idx == -1) return -ENOENT;

  /* Shift to fill hole */
  for (int i = idx; i < num_registered_backends - 1; i++) {
    registered_backends[i] = registered_backends[i + 1];
  }
  registered_backends[num_registered_backends - 1] = nullptr;
  num_registered_backends--;

  if (manual_backend == (const printk_backend_t *) backend) {
    manual_backend = nullptr;
  }

  /* If we removed the active backend, or if we are just ensuring state */
  if (active_backend == backend) {
    /* Force cleanup of the removed backend logic */
    if (backend->cleanup) backend->cleanup();
    active_backend = nullptr;

    /* Fallback to next best */
    printk_auto_configure(nullptr, 1);
  }

  return 0;
}
EXPORT_SYMBOL(printk_unregister_backend);

int __no_cfi printk_auto_configure(void *payload, const int reinit) {
  if (manual_backend) {
    /* User forced a backend, try to keep it */
    if (active_backend == manual_backend) return 0;

    /* We need a non-const pointer for backend_try_init, but manual_backend is const.
     * In this specific system, we know registered_backends contains the pointers.
     */
    printk_backend_t *target = nullptr;
    for (int i = 0; i < num_registered_backends; i++) {
      if (registered_backends[i] == (const printk_backend_t *) manual_backend) {
        target = registered_backends[i];
        break;
      }
    }

    if (target && backend_try_init(target, payload) == 0) {
      backend_set_active(target, reinit);
      return 0;
    }
    /* Manual backend failed, drop override and fall through */
    printk(KERN_ERR KERN_CLASS "printk: manual backend '%s' failed or pending, falling back\n",
           manual_backend->name);
    manual_backend = nullptr;
  }

  /* Iterate sorted list to find first working backend */
  for (int i = 0; i < num_registered_backends; i++) {
    printk_backend_t *b = registered_backends[i];

    if (b->state == PRINTK_BACKEND_PENDING) continue;

    /* Optimization: if already active, check if healthy?
     * For now, assume active implies healthy unless external event.
     */
    if (b == active_backend) return 0;

    if (backend_try_init(b, payload) == 0) {
      backend_set_active(b, reinit);
      return 0;
    }
  }

  /* No working backend found */
  if (active_backend) {
    /* If we had an active one but now nothing works (?), shutdown old one */
    if (active_backend->cleanup) active_backend->cleanup();
    active_backend = nullptr;
  }

  if (reinit) log_set_console_sink(nullptr, nullptr);
  else log_init(nullptr, nullptr);

  printk(KERN_ERR KERN_CLASS "printk: no active backend found, logging to kfifo\n");
  return -ENODEV;
}

int __no_cfi printk_set_sink(const char *backend_name, bool cleanup) {
  if (!backend_name) {
    printk_shutdown();
    return 0;
  }

  printk_backend_t *target = nullptr;
  for (int i = 0; i < num_registered_backends; i++) {
    if (strcmp(registered_backends[i]->name, backend_name) == 0) {
      target = registered_backends[i];
      break;
    }
  }

  if (!target) {
    printk(KERN_ERR KERN_CLASS "printk: backend '%s' not found\n", backend_name);
    return -ENOENT;
  }

  if (backend_try_init(target, nullptr) != 0) {
    printk(KERN_ERR KERN_CLASS "printk: failed to init '%s' (might be pending)\n", backend_name);
    return -EIO;
  }

  manual_backend = target; /* Set override */
  if (cleanup && active_backend && active_backend->cleanup) {
    active_backend->cleanup();
  }

  /* backend_set_active handles the switch */
  backend_set_active(target, 1);
  return 0;
}

EXPORT_SYMBOL(printk_set_sink);

void printk_disable(void) {
  if (printk_disabled) return;
  printk_disabled = true;
  /* We keep active_backend set so we know what to resume to,
   * but we detach the sink from the logger.
   */
  log_set_console_sink(nullptr, nullptr);
}

EXPORT_SYMBOL(printk_disable);

void printk_enable(void) {
  if (!printk_disabled) return;
  printk_disabled = false;

  /* Try to restore current active, or find best */
  if (active_backend) {
    log_set_console_sink(active_backend->putc, active_backend->write);
  } else {
    printk_auto_configure(nullptr, 1);
  }
}

EXPORT_SYMBOL(printk_enable);

const printk_backend_t *printk_auto_select_backend(const char *not) {
  for (int i = 0; i < num_registered_backends; i++) {
    const printk_backend_t *b = registered_backends[i];
    if (not && strcmp(b->name, not) == 0) continue;
    if (b->state == PRINTK_BACKEND_PENDING) continue;
    return b; /* List is sorted, return first match */
  }
  return nullptr;
}

EXPORT_SYMBOL(printk_auto_select_backend);

int printk_shutdown_backend(const printk_backend_t *b, const int force) {
  return printk_unregister_backend(b, force);
}

EXPORT_SYMBOL(printk_shutdown_backend);

void __no_cfi printk_shutdown(void) {
  if (active_backend && active_backend->cleanup) {
    active_backend->cleanup();
  }
  active_backend = nullptr;
  manual_backend = nullptr;
  log_set_console_sink(nullptr, nullptr);
}

EXPORT_SYMBOL(printk_shutdown);

#ifdef ASYNC_PRINTK
int printk_init_async(void) {
  printk(KERN_INFO KERN_CLASS "printk: starting async logger\n");
  return log_init_async();
}
#endif

/* --- String Formatting Helpers --- */

static const char *parse_level_prefix(const char *fmt, int *level_io) {
  if (!fmt || !fmt[0]) return fmt;
  /* Format: $<0-8>$ */
  if (fmt[0] == '$' && fmt[1] >= '0' && fmt[1] <= '8' && fmt[2] == '$') {
    if (level_io) *level_io = (fmt[1] - '0');
    return fmt + 3;
  }
  return fmt;
}

static int vprintk_internal(const char *fmt, va_list args, bool newline) {
  if (!fmt) return -EINVAL;

  int level = KLOG_INFO;
  const char *real_fmt = parse_level_prefix(fmt, &level);

  char local_buf[256];
  int count = vsnprintf(local_buf, sizeof(local_buf), real_fmt, args);

  if (newline) {
    if (count < (int) sizeof(local_buf) - 1) {
      local_buf[count++] = '\n';
      local_buf[count] = '\0';
    }
  }

  log_write_str(level, local_buf);
  return count;
}

int vprintk(const char *fmt, va_list args) {
  return vprintk_internal(fmt, args, false);
}

EXPORT_SYMBOL(vprintk);

int vprintkln(const char *fmt, va_list args) {
  return vprintk_internal(fmt, args, true);
}

EXPORT_SYMBOL(vprintkln);

int printk(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vprintk_internal(fmt, args, false);
  va_end(args);
  return ret;
}

EXPORT_SYMBOL(printk);

int printkln(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vprintk_internal(fmt, args, true);
  va_end(args);
  return ret;
}

EXPORT_SYMBOL(printkln);

int ___ratelimit(ratelimit_state_t *rs, const char *func) {
  if (!rs) return 1;

  irq_flags_t flags = spinlock_lock_irqsave(&rs->lock);
  uint64_t now = get_time_ns();
  uint64_t interval_ns = (uint64_t) rs->interval * 1000000ULL;

  if (!rs->begin || (now - rs->begin) >= interval_ns) {
    if (rs->missed > 0) {
      /* Use raw printk to avoid infinite recursion */
      char buf[128];
      snprintf(buf, sizeof(buf), KERN_WARNING KERN_CLASS "%d messages suppressed by %s\n", rs->missed, func);
      log_write_str(KLOG_WARNING, buf);
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
