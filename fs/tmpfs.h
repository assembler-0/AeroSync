/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/tmpfs.h
 * @brief Internal tmpfs declarations
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <fs/vfs.h>

struct tmpfs_node;
extern struct file_system_type tmpfs_type;

int tmpfs_fill_super(struct super_block *sb, void *data);
int tmpfs_create_kern(struct super_block *sb, struct tmpfs_node *parent, const char *name, vfs_mode_t mode, dev_t dev);
struct tmpfs_node *tmpfs_mkdir_kern(struct super_block *sb, struct tmpfs_node *parent, const char *name, vfs_mode_t mode);
int tmpfs_remove_kern(struct super_block *sb, struct tmpfs_node *parent, const char *name);
