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
#include <kernel/mutex.h>
#include <lib/printk.h>
#include <kernel/classes.h>

// Global lists for VFS objects
LIST_HEAD(super_blocks);  // List of all mounted superblocks
LIST_HEAD(inodes);        // List of all active inodes
LIST_HEAD(dentries);      // List of all active dentries (dentry cache)

// Mutexes to protect global lists
static struct mutex sb_mutex;
static struct mutex inode_mutex;
static struct mutex dentry_mutex;

LIST_HEAD(file_systems); // List of all registered file system types
static struct mutex fs_type_mutex;

void vfs_init(void) {
    printk(VFS_CLASS "Initializing Virtual File System...\n");

    // Initialize global lists
    INIT_LIST_HEAD(&super_blocks);
    INIT_LIST_HEAD(&inodes);
    INIT_LIST_HEAD(&dentries);
    INIT_LIST_HEAD(&file_systems);

    // Initialize mutexes
    mutex_init(&sb_mutex);
    mutex_init(&inode_mutex);
    mutex_init(&dentry_mutex);
    mutex_init(&fs_type_mutex);

    printk(VFS_CLASS "Initialization complete.\n");
}

// Function to register a new filesystem type
int register_filesystem(struct file_system_type *fs) {
    if (!fs || !fs->name || !fs->mount || !fs->kill_sb) {
        printk(KERN_ERR VFS_CLASS "ERROR: Attempted to register an invalid filesystem type.\n");
        return -1;
    }
    mutex_lock(&fs_type_mutex);
    list_add_tail(&fs->fs_list, &file_systems);
    mutex_unlock(&fs_type_mutex);
    printk(VFS_CLASS "Registered filesystem: %s\n", fs->name);
    return 0;
}

// Function to unregister a filesystem type
int unregister_filesystem(struct file_system_type *fs) {
    if (!fs) {
        printk(KERN_ERR VFS_CLASS "ERROR: Attempted to unregister a NULL filesystem type.\n");
        return -1;
    }
    mutex_lock(&fs_type_mutex);
    list_del(&fs->fs_list);
    mutex_unlock(&fs_type_mutex);
    printk(VFS_CLASS "Unregistered filesystem: %s\n", fs->name);
    return 0;
}
