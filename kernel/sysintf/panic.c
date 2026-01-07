/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file kernel/sysintf/panic.c
 * @brief Kernel panic handlers management system
 * @copyright (C) 2025 assembler-0
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

#define MAX_PANIC_HANDLERS 8

#include <lib/string.h>
#include <vsprintf.h>
#include <kernel/classes.h>
#include <kernel/fkx/fkx.h>
#include <kernel/sysintf/panic.h>

static const panic_ops_t *registered_backends[MAX_PANIC_HANDLERS];
static int num_registered_backends = 0;
static const panic_ops_t *active_backend = NULL;

void panic_register_handler(const panic_ops_t *ops) {
  if (num_registered_backends >= MAX_PANIC_HANDLERS) {
    // Can't printk here safely maybe?
    return;
  }
  registered_backends[num_registered_backends++] = ops;
}

void panic_handler_install() {
  const panic_ops_t *best = NULL;

  for (int i = 0; i < num_registered_backends; i++) {
    const panic_ops_t *b = registered_backends[i];
    if (!b)
      continue;

    if (!best || b->prio > best->prio)
      best = b;
  }

  if (!best) {
    active_backend = NULL;
    return;
  }
  active_backend = best;
}

void panic_switch_handler(const char *name) {
  if (!name) return;
  for (int i = 0; i < num_registered_backends; i++) {
    const panic_ops_t *b = registered_backends[i];
    if (b && b->name && strcmp(b->name, name) == 0) {
      if (active_backend && active_backend->cleanup) {
        // null dereference check - not all backends implement cleanup
        active_backend->cleanup();
      }
      if (b->init) {
        // same as above
        if (b->init() != 0) {
          printk(KERN_ERR KERN_CLASS "failed to reinit printk backend %s\n", name);
          return;
        }
      }
      active_backend = b;
    }
  }
}

void __exit __noinline __noreturn __sysv_abi panic(const char *msg, ...) {
  va_list va;
  va_start(va, msg);
  char buff[128];
  vsnprintf(buff, sizeof(buff), msg, va);
  active_backend->panic(buff);
  va_end(va);
}

void __exit __noinline __noreturn __sysv_abi panic_exception(cpu_regs *regs) {
  active_backend->panic_exception(regs);
}

void __exit __noinline __noreturn __sysv_abi panic_early() {
  active_backend->panic_early();
}