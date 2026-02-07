/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/pseudo_fs.h
 * @brief Generic Pseudo-Filesystem Library
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <fs/vfs.h>
#include <linux/list.h>

struct pseudo_node;

/**
 * struct pseudo_fs_info - Instance of a pseudo filesystem
 */
struct pseudo_fs_info {
    const char *name;
    struct super_block *sb;
    struct pseudo_node *root;
    struct file_system_type fs_type;
};

/**
 * struct pseudo_node - Entry in a pseudo filesystem tree
 */
struct pseudo_node {
    char name[64];
    vfs_mode_t mode;
    struct inode *inode;
    void *private_data;
    const struct file_operations *fop;
    const struct inode_operations *iop;
    
    /* Callback for custom inode initialization (e.g. for devfs) */
    void (*init_inode)(struct inode *inode, struct pseudo_node *node);

    struct pseudo_node *parent;
    struct list_head children;
    struct list_head sibling;
};

/**
 * pseudo_fs_register - Register a new pseudo-FS type (e.g., "resfs")
 */
int pseudo_fs_register(struct pseudo_fs_info *fs);

/**
 * pseudo_fs_create_node - Create a file or directory in the pseudo-FS
 */
struct pseudo_node *pseudo_fs_create_node(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          vfs_mode_t mode,
                                          const struct file_operations *fops,
                                          void *private_data);

/**
 * pseudo_fs_remove_node - Remove a node from the tree
 */
void pseudo_fs_remove_node(struct pseudo_fs_info *fs, struct pseudo_node *node);
