/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/limine_modules.h
 * @brief Limine Module Manager (LMM) API
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <limine/limine.h>
#include <aerosync/types.h>
#include <linux/list.h>

/**
 * @brief Module type identifiers
 */
typedef enum {
  LMM_TYPE_UNKNOWN = 0,
  LMM_TYPE_FKX,     /* AeroSync Kernel Extension */
  LMM_TYPE_ASRX,    /* AeroSync Resource/Archive (future) */
  LMM_TYPE_INITRD,  /* Initial Ramdisk (CPIO) */
  LMM_TYPE_FIRMWARE, /* Firmware blobs */
  LMM_TYPE_CONFIG,   /* Configuration files */
  LMM_TYPE_CUSTOM,   /* User-defined types */
  LMM_TYPE_MAX
} lmm_type_t;

/**
 * @brief Module entry structure
 */
struct lmm_entry {
  const struct limine_file *file;
  lmm_type_t type;
  uint32_t priority;
  struct list_head list;
  void *priv; /* Subsystem-specific data */
};

/**
 * @brief Prober function signature
 * @return Positive score (0-100) if recognized, 0 if not recognized, negative on error.
 */
typedef int (*lmm_prober_fn)(const struct limine_file *file, lmm_type_t *out_type);

/**
 * @brief Register a prober for a module type
 */
int lmm_register_prober(lmm_prober_fn prober);

/**
 * @brief Initialize the module manager and scan Limine modules
 */
int lmm_init(const struct limine_module_response *response);

/**
 * @brief Iterate over all modules of a specific type
 */
void lmm_for_each_module(lmm_type_t type, void (*callback)(struct lmm_entry *entry, void *data), void *data);

/**
 * @brief Find a module by name (path, normalized path, or basename)
 */
struct lmm_entry *lmm_find_module(const char *name);

/**
 * @brief Find the first module of a specific type
 */
struct lmm_entry *lmm_find_module_by_type(lmm_type_t type);

/**
 * @brief Get the total count of modules
 */
size_t lmm_get_count(void);
