/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/devfs.c
 * @brief Device Filesystem (devfs) implementation
 * @copyright (C) 2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <fs/pseudo_fs.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <aerosync/export.h>
#include <aerosync/mutex.h>
#include <lib/printk.h>

#ifdef CONFIG_DEVFS_USE_PSEUDO_FS
static void devfs_init_inode(struct inode *inode, struct pseudo_node *node) {
  dev_t rdev = (dev_t) (uintptr_t) node->private_data;
  extern void init_special_inode(struct inode *inode, vfs_mode_t mode, dev_t rdev);
  init_special_inode(inode, node->mode, rdev);
}

static struct pseudo_fs_info devfs_info = {
  .name = "devfs",
};
#else
struct devfs_entry {
  char name[64];
  vfs_mode_t mode;
  dev_t rdev;
  struct list_head list;
};

static LIST_HEAD(devfs_entries);
static struct mutex devfs_lock;
static struct super_block *devfs_sb = nullptr;
#endif

#ifndef CONFIG_DEVFS_USE_PSEUDO_FS
static struct dentry *devfs_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) dir;
  (void) flags;
  struct devfs_entry *entry;

  mutex_lock(&devfs_lock);
  list_for_each_entry(entry, &devfs_entries, list)
  {
    if (strcmp((const char *) dentry->d_name.name, entry->name) == 0) {
      struct inode *inode = kzalloc(sizeof(struct inode));
      if (!inode) {
        mutex_unlock(&devfs_lock);
        return nullptr;
      }

      inode->i_sb = devfs_sb;
      inode->i_ino = (uint64_t) entry; // Dummy inode number
      extern void init_special_inode(struct inode *inode, vfs_mode_t mode, dev_t rdev);
      init_special_inode(inode, entry->mode, entry->rdev);

      dentry->d_inode = inode;
      mutex_unlock(&devfs_lock);
      return dentry;
    }
  }
  mutex_unlock(&devfs_lock);

  return nullptr;
}

static struct inode_operations devfs_dir_inode_ops = {
  .lookup = devfs_lookup,
};

static int devfs_mount(struct file_system_type *fs_type, const char *dev_name, const char *dir_name,
                       unsigned long flags, void *data) {
  (void) dev_name;
  (void) dir_name;
  (void) flags;
  (void) data;

  struct super_block *sb = kzalloc(sizeof(struct super_block));
  if (!sb) return -1;

  struct inode *inode = kzalloc(sizeof(struct inode));
  if (!inode) {
    kfree(sb);
    return -1;
  }

  inode->i_mode = S_IFDIR | 0755;
  inode->i_op = &devfs_dir_inode_ops;
  inode->i_sb = sb;

  struct qstr root_name = {.name = (const unsigned char *) "dev", .len = 3};
  extern struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);
  sb->s_root = d_alloc_pseudo(sb, &root_name);
  sb->s_root->d_inode = inode;

  devfs_sb = sb;
  return 0;
}

static void devfs_kill_sb(struct super_block *sb) {
  if (sb) {
    if (sb->s_root) {
      if (sb->s_root->d_inode) kfree(sb->s_root->d_inode);
      kfree(sb->s_root);
    }
    kfree(sb);
  }
}

static struct file_system_type devfs_type = {
  .name = "devfs",
  .mount = devfs_mount,
  .kill_sb = devfs_kill_sb
};
#endif

void devfs_init(void) {
#ifdef CONFIG_DEVFS_USE_PSEUDO_FS
  if (pseudo_fs_register(&devfs_info) != 0) {
    printk(KERN_ERR DEVFS_CLASS "Failed to register via Pseudo-FS\n");
  }
#else
  mutex_init(&devfs_lock);
  register_filesystem(&devfs_type);
#endif
}

EXPORT_SYMBOL(devfs_init);


int devfs_register_device(const char *name, vfs_mode_t mode, dev_t dev) {
#ifdef CONFIG_DEVFS_USE_PSEUDO_FS
  struct pseudo_node *node = pseudo_fs_create_node(&devfs_info, nullptr, name, mode, nullptr, (void *) (uintptr_t) dev);
  if (!node) return -ENOMEM;

  node->init_inode = devfs_init_inode;
#else
  struct devfs_entry *entry = kzalloc(sizeof(struct devfs_entry));
  if (!entry) return -1;

  strncpy(entry->name, name, sizeof(entry->name));
  entry->mode = mode;
  entry->rdev = dev;

  mutex_lock(&devfs_lock);
  list_add_tail(&entry->list, &devfs_entries);
  mutex_unlock(&devfs_lock);
#endif
  return 0;
}

EXPORT_SYMBOL(devfs_register_device);
