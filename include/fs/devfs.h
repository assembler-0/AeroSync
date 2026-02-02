/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/devfs.h
 * @brief Device Filesystem Interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <fs/vfs.h>

/**
 * devfs_init - Initialize DevFS
 */
void devfs_init(void);

/**
 * devfs_register_device - Create a device node in DevFS
 * @name: Name of the device (e.g., "ttyS0")
 * @mode: File mode (e.g., S_IFCHR | 0666)
 * @dev: Device number
 */
int devfs_register_device(const char *name, vfs_mode_t mode, dev_t dev);
