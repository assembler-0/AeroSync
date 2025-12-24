/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file lib/printk.c
 * @brief printk backend management and logging functions
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <arch/x64/io.h>
#include <lib/log.h>
#include <kernel/classes.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <lib/string.h>

#define MAX_PRINTK_BACKENDS 8

static const printk_backend_t *registered_backends[MAX_PRINTK_BACKENDS];
static int num_registered_backends = 0;
static const printk_backend_t *active_backend = NULL;

void printk_register_backend(const printk_backend_t *backend) {
  if (num_registered_backends >= MAX_PRINTK_BACKENDS) {
    // Can't printk here safely maybe? or fallback to internal ring
    return;
  }
  registered_backends[num_registered_backends++] = backend;
}

void printk_auto_configure(void *payload, const int reinit) {
  const printk_backend_t *best = NULL;

  for (int i = 0; i < num_registered_backends; i++) {
    const printk_backend_t *b = registered_backends[i];
    if (!b)
      continue;

    if (!b->probe())
      continue;

    if (b->init(payload) != 0)
      continue;

    if (!best || b->priority > best->priority)
      best = b;
  }

  if (!best) {
    // Fallback to internal ringbuffer only
    if (reinit) log_set_console_sink(NULL);
    else log_init(NULL);
    printk(KERN_ERR KERN_CLASS "no active printk backend, logging to ringbuffer only\n");
    active_backend = NULL;
    return;
  }

  printk(KERN_INFO KERN_CLASS
         "printk backend selected: %s (prio=%d)\n",
         best->name, best->priority);
  active_backend = best;
  if (reinit) log_set_console_sink(best->putc);
  else log_init(best->putc);
}

void printk_init_async(void) {
  printk(KERN_CLASS "Enabling asynchronous printk...\n");
  log_init_async();
}

int printk_set_sink(const char *backend_name) {
  if (!backend_name) return -1;

  for (int i = 0; i < num_registered_backends; i++) {
    const printk_backend_t *b = registered_backends[i];
    if (b && b->name && strcmp(b->name, backend_name) == 0) {
      if (active_backend && active_backend->cleanup) {
        // null dereference check - not all backends implement cleanup
        active_backend->cleanup();
      }

      if (b->is_active && b->init) {
        // same as above
        if (!b->is_active() && b->init(NULL) != 0) {
          printk(KERN_ERR KERN_CLASS "failed to reinit printk backend %s\n", backend_name);
          return -1;
        }
      }

      log_set_console_sink(b->putc);
      active_backend = b;
      return 0;
    }
  }
  return -1; // Not found
}

void printk_shutdown(void) {
  if (active_backend && active_backend->cleanup) {
    active_backend->cleanup();
  }
  active_backend = NULL;
  log_set_console_sink(NULL);
}

static const char *parse_level_prefix(const char *fmt, int *level_io) {
  // Safety check for null or too-short strings
  if (!fmt || !fmt[0] || !fmt[1] || !fmt[2])
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
    return -1;

  int level = KLOG_INFO; // Default log level
  // Parse optional level prefix (e.g. "$3$")
  const char *real_fmt = parse_level_prefix(fmt, &level);

  char local_buf[512];
  int count = vsnprintf(local_buf, sizeof(local_buf), real_fmt, args);

  // Write to log subsystem
  log_write_str(level, local_buf);

  return count;
}

int printk(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vprintk(fmt, args);
  va_end(args);
  return ret;
}
