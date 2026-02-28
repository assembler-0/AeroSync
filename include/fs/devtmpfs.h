/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/devtmpfs.h
 * @brief devtmpfs public interface
 * @copyright (C) 2026 assembler-0
 */

#ifndef AEROSYNC_FS_DEVTMPFS_H
#define AEROSYNC_FS_DEVTMPFS_H

#include <fs/vfs.h>

void devtmpfs_init(void);

int devtmpfs_register_device(const char *name, const char *category, vfs_mode_t mode, dev_t dev);

#endif /* AEROSYNC_FS_DEVTMPFS_H */
