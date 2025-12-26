/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/fkx/fkx.h
 * @brief FKX Module Interface Definitions
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

#pragma once

#include <kernel/sysintf/time.h>
#include <kernel/sysintf/ic.h>
#include <lib/printk.h>
#include <arch/x64/cpu.h>
#include <kernel/mutex.h>
#include <kernel/semaphore.h>
#include <kernel/wait.h>
#include <kernel/errno.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>

/* FKX Magic: "FKX1" in little-endian */
#define FKX_MAGIC 0x31584B46

/* FKX Module API Version */
#define FKX_API_VERSION 1

/* Module flags */
#define FKX_FLAG_REQUIRED    (1 << 0)  /* System cannot boot without this module */
#define FKX_FLAG_CORE        (1 << 1)  /* Core system component */

/* Return codes */
#define FKX_SUCCESS          0
// use errno.h!

/**
 * Kernel Symbol structure
 */
struct fkx_symbol {
    uintptr_t addr;
    const char *name;
};

/**
 * EXPORT_SYMBOL - Export a symbol to the global kernel symbol table
 *
 * This macro places symbol information into a dedicated section that
 * the FKX loader can parse.
 */
#define EXPORT_SYMBOL(sym) \
    static const char __fkx_sym_name_##sym[] = #sym; \
    __attribute__((section("fkx_ksymtab"), used)) \
    const struct fkx_symbol __fkx_sym_##sym = { \
        .addr = (uintptr_t)&sym, \
        .name = __fkx_sym_name_##sym \
    }

typedef enum {
  FKX_PRINTK_CLASS,
  FKX_DRIVER_CLASS,
  FKX_IC_CLASS,
  FKX_TIMER_CLASS,
  FKX_MM_CLASS,
  FKX_GENERIC_CLASS,
  FKX_MAX_CLASS,
} fkx_module_class_t;

/* Forward declarations */
struct fkx_kernel_api;
struct fkx_module_info;

/**
 * Module entry point signature
 *
 * @return FKX_SUCCESS on success, negative error code on failure
 */
typedef int (*fkx_entry_fn)(void);

/**
 * Module Information Structure
 *
 * Must be present in every FKX module at a well-known location.
 * Typically placed in a dedicated section (.fkx_info)
 */
struct fkx_module_info {
  uint32_t magic; /* Must be FKX_MAGIC */
  uint32_t api_version; /* FKX_API_VERSION this module was built for */

  const char *name; /* Module name (null-terminated) */
  const char *version; /* Module version string */
  const char *author; /* Author/vendor */
  const char *description; /* Brief description */

  uint32_t flags; /* FKX_FLAG_* combination */
  fkx_module_class_t module_class;

  /* Entry point */
  fkx_entry_fn init;

  /* Dependencies (null-terminated array of module names) */
  const char **depends;

  /* Reserved for future use */
  void *reserved_ptr[4];
};

/**
 * FKX_MODULE_DEFINE - Convenience macro to define module info
 *
 * Usage:
 *   FKX_MODULE_DEFINE(
 *       my_module,
 *       "1.0.0",
 *       "Author Name",
 *       "Module description",
 *       FKX_FLAG_CORE,
 *       my_module_init,
 *       my_module_deps
 *   );
 */
#define FKX_MODULE_DEFINE(_name, ver, auth, desc, flg, cls, entry, deps) \
    __attribute__((section(".fkx_info"), used)) struct fkx_module_info __fkx_module_info_##_name = { \
        .magic = FKX_MAGIC, \
        .api_version = FKX_API_VERSION, \
        .name = #_name, \
        .version = ver, \
        .author = auth, \
        .description = desc, \
        .flags = flg, \
        .module_class = cls, \
        .init = entry, \
        .depends = deps, \
        .reserved_ptr = {0} \
    }

/**
 * FKX_NO_DEPENDENCIES - Use when module has no dependencies
 */

/**
 * Load an FKX module image into memory without calling init
 *
 * @param data Pointer to the start of the ELF module
 * @param size Size of the module in bytes
 * @return FKX_SUCCESS on success, error code otherwise
 */
int fkx_load_image(void *data, size_t size);

/**
 * Lookup a symbol in the global kernel symbol table
 *
 * @param name Name of the symbol
 * @return Address of the symbol or 0 if not found
 */
uintptr_t fkx_lookup_symbol(const char *name);

/**
 * Register a new symbol in the global kernel symbol table
 *
 * @param addr Address of the symbol
 * @param name Name of the symbol
 * @return 0 on success, error code otherwise
 */
int fkx_register_symbol(uintptr_t addr, const char *name);

/**
 * Initialize all modules of a specific class
 *
 * @param module_class The class of modules to initialize
 * @return FKX_SUCCESS on success, error code otherwise
 */
int fkx_init_module_class(fkx_module_class_t module_class);

/**
 * Finalize loading of all modules (resolve dependencies and relocations)
 * 
 * @return FKX_SUCCESS on success, error code otherwise
 */
int fkx_finalize_loading(void);
