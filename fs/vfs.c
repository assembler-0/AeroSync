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
#include <fs/file.h>
#include <include/linux/list.h>
#include <kernel/types.h>
#include <kernel/fkx/fkx.h>
#include <kernel/mutex.h>
#include <lib/printk.h>
#include <kernel/classes.h>
#include <mm/slab.h>

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

extern struct dentry *root_dentry;

static ssize_t rootfs_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
    (void)file; (void)buf; (void)count; (void)ppos;
    return 0; // EOF
}

static vfs_off_t rootfs_llseek(struct file *file, vfs_off_t offset, int whence) {
    vfs_loff_t new_pos = file->f_pos;
    switch (whence) {
        case 0: new_pos = offset; break;
        case 1: new_pos += offset; break;
        case 2: new_pos = offset; break; // size is 0
        default: return -1;
    }
    if (new_pos < 0) return -1;
    file->f_pos = new_pos;
    return new_pos;
}

static struct file_operations rootfs_dir_operations = {
    .read = rootfs_read,
    .llseek = rootfs_llseek,
};

// Dummy rootfs implementation
static int rootfs_mount(struct file_system_type *fs_type, const char *dev_name, const char *dir_name, unsigned long flags, void *data) {
    struct super_block *sb = kzalloc(sizeof(struct super_block));
    if (!sb) return -1;

    sb->s_op = NULL; // Add dummy ops if needed
    
    struct inode *inode = kzalloc(sizeof(struct inode));
    if (!inode) {
        kfree(sb);
        return -1;
    }

    inode->i_ino = 1;
    inode->i_mode = S_IFDIR | 0755;
    inode->i_sb = sb;
    inode->i_fop = &rootfs_dir_operations;

    struct qstr root_name = {.name = (const unsigned char *)"/", .len = 1};
    extern struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);
    root_dentry = d_alloc_pseudo(sb, &root_name);
    if (!root_dentry) {
        kfree(inode);
        kfree(sb);
        return -1;
    }
    root_dentry->d_inode = inode;
    sb->s_root = root_dentry;

    list_add_tail(&sb->sb_list, &super_blocks);
    return 0;
}

static void rootfs_kill_sb(struct super_block *sb) {
    // Nothing to do for rootfs
}

static struct file_system_type rootfs_type = {
    .name = "rootfs",
    .mount = rootfs_mount,
    .kill_sb = rootfs_kill_sb
};

void vfs_init(void) {
    printk(VFS_CLASS "Initializing Virtual File System...\n");

    extern void files_init(void);
    files_init();

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

    register_filesystem(&rootfs_type);
    rootfs_mount(&rootfs_type, NULL, "/", 0, NULL);

    printk(VFS_CLASS "Initialization complete.\n");
}
EXPORT_SYMBOL(vfs_init);

struct file *vfs_open(const char *path, int flags, int mode) {
    struct dentry *dentry = vfs_path_lookup(path, 0);
    if (!dentry) return NULL;

    struct inode *inode = dentry->d_inode;
    if (!inode) return NULL;

    struct file *file = kzalloc(sizeof(struct file));
    if (!file) return NULL;

    file->f_dentry = dentry;
    file->f_inode = inode;
    file->f_op = inode->i_fop;
    file->f_flags = flags;
    file->f_pos = 0;
    atomic_set(&file->f_count, 1);

    if (file->f_op && file->f_op->open) {
        int ret = file->f_op->open(inode, file);
        if (ret < 0) {
            kfree(file);
            return NULL;
        }
    }

    return file;
}
EXPORT_SYMBOL(vfs_open);

ssize_t vfs_read(struct file *file, char *buf, size_t count, vfs_loff_t *pos) {
    if (!file || !file->f_op || !file->f_op->read) return -1;
    return file->f_op->read(file, buf, count, pos);
}
EXPORT_SYMBOL(vfs_read);

ssize_t vfs_write(struct file *file, const char *buf, size_t count, vfs_loff_t *pos) {
    if (!file || !file->f_op || !file->f_op->write) return -1;
    return file->f_op->write(file, buf, count, pos);
}
EXPORT_SYMBOL(vfs_write);

int vfs_close(struct file *file) {
    if (!file) return -1;
    if (file->f_op && file->f_op->release) {
        file->f_op->release(file->f_inode, file);
    }
    kfree(file);
    return 0;
}
EXPORT_SYMBOL(vfs_close);

vfs_loff_t vfs_llseek(struct file *file, vfs_loff_t offset, int whence) {
    if (!file) return -1;
    if (file->f_op && file->f_op->llseek) {
        return file->f_op->llseek(file, offset, whence);
    }

    // Default implementation
    vfs_loff_t new_pos = file->f_pos;
    switch (whence) {
        case 0: // SEEK_SET
            new_pos = offset;
            break;
        case 1: // SEEK_CUR
            new_pos += offset;
            break;
        case 2: // SEEK_END
            new_pos = file->f_inode->i_size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos < 0) return -1;
    file->f_pos = new_pos;
    return new_pos;
}
EXPORT_SYMBOL(vfs_llseek);

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
EXPORT_SYMBOL(register_filesystem);

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
EXPORT_SYMBOL(unregister_filesystem);
