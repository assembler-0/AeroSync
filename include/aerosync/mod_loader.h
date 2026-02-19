/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/mod_loader.h
 * @brief Common Module Loader Utilities (DRY Refactoring)
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/elf.h>
#include <aerosync/ksymtab.h>

struct mod_image {
  void *raw_data;
  size_t raw_size;
  
  void *base_addr;
  size_t total_size;
  uint64_t load_bias;
  uint64_t min_vaddr;

  uint32_t license;
  const char *name;
};

/**
 * Verify HMAC-SHA512 signature of a module
 */
int mod_verify_signature(void *data, size_t size, const uint8_t *key, size_t key_size);

/**
 * Map ELF segments into vmalloc_exec memory
 */
int mod_map_segments(struct mod_image *img);

/**
 * Perform x86_64 relocations
 */
int mod_relocate(struct mod_image *img);

/**
 * Register symbols from SHT_SYMTAB and ksymtab
 */
int mod_register_symbols(struct mod_image *img);

/**
 * Apply RX/RW/RO protections to segments
 */
int mod_apply_protections(struct mod_image *img);

/**
 * Common cleanup for failed loads
 */
void mod_cleanup_image(struct mod_image *img);
