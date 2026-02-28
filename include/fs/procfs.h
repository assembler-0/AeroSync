/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/procfs.h
 * @brief procfs public interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <fs/vfs.h>

void procfs_init(void);

int procfs_create_file_kern(const char *name, const struct file_operations *fops, void *private_data);
