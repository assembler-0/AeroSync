/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/ksymtab.c
 * @brief Kernel symbol table helpers
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

#include <aerosync/classes.h>
#include <aerosync/ksymtab.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <lib/printk.h>

extern const struct ksymbol _ksymtab_start[];
extern const struct ksymbol _ksymtab_end[];

struct dyn_ksymbol {
  struct ksymbol sym;
  struct dyn_ksymbol *next;
};

static struct dyn_ksymbol *g_dyn_symbols = NULL;

uintptr_t lookup_ksymbol(const char *name) {
  // 1. Search static kernel symbols
  const struct ksymbol *curr = _ksymtab_start;
  while (curr < _ksymtab_end) {
    if (strcmp(curr->name, name) == 0) {
      return curr->addr;
    }
    curr++;
  }

  // 2. Search dynamic module symbols
  struct dyn_ksymbol *dyn = g_dyn_symbols;
  while (dyn) {
    if (strcmp(dyn->sym.name, name) == 0) {
      return dyn->sym.addr;
    }
    dyn = dyn->next;
  }

  return 0;
}

int register_ksymbol(uintptr_t addr, const char *name) {
  if (!name) return -1;

  // Check if symbol already exists
  if (lookup_ksymbol(name)) {
    printk(KERN_WARNING FKX_CLASS "Symbol %s already registered, skipping\n", name);
    return -1;
  }

  struct dyn_ksymbol *new_sym = kmalloc(sizeof(struct dyn_ksymbol));
  if (!new_sym) return -1;

  new_sym->sym.addr = addr;
  new_sym->sym.name = name; // We assume the name string is persistent (e.g., in module's rodata)
  new_sym->next = g_dyn_symbols;
  g_dyn_symbols = new_sym;

  return 0;
}
