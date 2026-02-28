/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/mm/vfs.h
 * @brief Memory Management VFS integration interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

/**
 * mm_vfs_init - Initialize Memory Management VFS interfaces (sysfs, procfs)
 */
int mm_vfs_init(void);
