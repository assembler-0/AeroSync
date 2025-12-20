/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file fs/vfs.c
 * @brief Virtual File System core implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <fs/vfs.h>
#include <include/linux/list.h>
#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <kernel/classes.h>

// Global lists for VFS objects
LIST_HEAD(super_blocks);  // List of all mounted superblocks
LIST_HEAD(inodes);        // List of all active inodes
LIST_HEAD(dentries);      // List of all active dentries (dentry cache)

// Spinlocks to protect global lists
static spinlock_t sb_lock;
static spinlock_t inode_lock;
static spinlock_t dentry_lock;

LIST_HEAD(file_systems); // List of all registered file system types
static spinlock_t fs_type_lock;

void vfs_init(void) {
    printk(VFS_CLASS "Initializing Virtual File System...\n");

    // Initialize global lists
    INIT_LIST_HEAD(&super_blocks);
    INIT_LIST_HEAD(&inodes);
    INIT_LIST_HEAD(&dentries);
    INIT_LIST_HEAD(&file_systems);

    // Initialize spinlocks
    spinlock_init(&sb_lock);
    spinlock_init(&inode_lock);
    spinlock_init(&dentry_lock);
    spinlock_init(&fs_type_lock);

    printk(VFS_CLASS "Initialization complete.\n");
}

// Function to register a new filesystem type
int register_filesystem(struct file_system_type *fs) {
    if (!fs || !fs->name || !fs->mount || !fs->kill_sb) {
        printk(KERN_ERR VFS_CLASS "ERROR: Attempted to register an invalid filesystem type.\n");
        return -1;
    }
    spinlock_lock(&fs_type_lock);
    list_add_tail(&fs->fs_list, &file_systems);
    spinlock_unlock(&fs_type_lock);
    printk(VFS_CLASS "Registered filesystem: %s\n", fs->name);
    return 0;
}

// Function to unregister a filesystem type
int unregister_filesystem(struct file_system_type *fs) {
    if (!fs) {
        printk(KERN_ERR VFS_CLASS "ERROR: Attempted to unregister a NULL filesystem type.\n");
        return -1;
    }
    spinlock_lock(&fs_type_lock);
    list_del(&fs->fs_list);
    spinlock_unlock(&fs_type_lock);
    printk(VFS_CLASS "Unregistered filesystem: %s\n", fs->name);
    return 0;
}
