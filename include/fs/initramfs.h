/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/initramfs.h
 * @brief Initramfs (CPIO) support
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/limine_modules.h>

/*
 * CPIO NEWC header structure
 * All fields are hexadecimal characters
 */
struct cpio_newc_header {
  char c_magic[6];
  char c_ino[8];
  char c_mode[8];
  char c_uid[8];
  char c_gid[8];
  char c_nlink[8];
  char c_mtime[8];
  char c_filesize[8];
  char c_devmajor[8];
  char c_devminor[8];
  char c_rdevmajor[8];
  char c_rdevminor[8];
  char c_namesize[8];
  char c_check[8];
} __packed;

#define CPIO_NEWC_MAGIC "070701"

/**
 * @brief Unpack an initramfs CPIO archive into the root filesystem
 * 
 * @param data Pointer to the CPIO archive data
 * @param size Size of the CPIO archive data
 * @return 0 on success, negative error code on failure
 */
int initramfs_unpack(void *data, size_t size);

/**
 * @brief Prober for CPIO archives
 */
int initramfs_cpio_prober(const struct limine_file *file, lmm_type_t *out_type);

/**
 * @brief Find and unpack the initramfs module
 * @param initrd_name Name of the initramfs module to load
 * This function uses LMM to find the module with the specified name
 * and unpacks it if found.
 */
void initramfs_init(const char *initrd_name);
