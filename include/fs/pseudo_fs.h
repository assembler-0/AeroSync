/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/pseudo_fs.h
 * @brief Generic Pseudo-Filesystem Library
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/rw_semaphore.h>
#include <fs/vfs.h>
#include <linux/rbtree.h>

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
    vfs_ino_t i_ino;
    struct inode *inode;
    void *private_data;
    const struct file_operations *fop;
    const struct inode_operations *iop;
    char *symlink_target; /* For symlinks */
    
    /* Callback for custom inode initialization (e.g. for devtmpfs) */
    void (*init_inode)(struct inode *inode, struct pseudo_node *node);

    struct pseudo_node *parent;
    struct rb_root children; /* RB-Tree root for children */
    struct rb_node rb_node;  /* Node in parent's tree */
    struct list_head sibling; /* Fallback list for iteration if needed */
    
    struct rw_semaphore lock; /* Protects children */
    struct resdomain *rd;     /* Resource domain to charge */
};

/**
 * pseudo_fs_register - Register a new pseudo-FS type (e.g., "resfs")
 */
int pseudo_fs_register(struct pseudo_fs_info *fs);

/**
 * pseudo_fs_find_node - Find a node by name in a parent
 */
struct pseudo_node *pseudo_fs_find_node(struct pseudo_node *parent, const char *name);

/**
 * pseudo_fs_create_node - Generic creation function
 */
struct pseudo_node *pseudo_fs_create_node(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          vfs_mode_t mode,
                                          const struct file_operations *fops,
                                          void *private_data);

/* Helper functions for specific types */
struct pseudo_node *pseudo_fs_create_dir(struct pseudo_fs_info *fs,
                                         struct pseudo_node *parent,
                                         const char *name);

struct pseudo_node *pseudo_fs_create_file(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          const struct file_operations *fops,
                                          void *private_data);

struct pseudo_node *pseudo_fs_create_link(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          const char *target);

/**
 * pseudo_fs_remove_node - Remove a node from the tree
 */
void pseudo_fs_remove_node(struct pseudo_fs_info *fs, struct pseudo_node *node);
