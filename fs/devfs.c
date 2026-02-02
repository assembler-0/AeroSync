/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/devfs.c
 * @brief Device Filesystem (devfs) implementation
 * @copyright (C) 2026 assembler-0
 */

#include <fs/devfs.h>
#include <fs/vfs.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <aerosync/export.h>
#include <aerosync/mutex.h>

struct devfs_entry {
  char name[64];
  vfs_mode_t mode;
  dev_t rdev;
  struct list_head list;
};

static LIST_HEAD(devfs_entries);
static struct mutex devfs_lock;

static struct super_block *devfs_sb = nullptr;

static struct dentry *devfs_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) dir;
  (void) flags;
  struct devfs_entry *entry;

  mutex_lock(&devfs_lock);
  list_for_each_entry(entry, &devfs_entries, list) {
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

void devfs_init(void) {
  mutex_init(&devfs_lock);
  register_filesystem(&devfs_type);
}

EXPORT_SYMBOL(devfs_init);


int devfs_register_device(const char *name, vfs_mode_t mode, dev_t dev) {
  struct devfs_entry *entry = kzalloc(sizeof(struct devfs_entry));
  if (!entry) return -1;

  strncpy(entry->name, name, sizeof(entry->name));
  entry->mode = mode;
  entry->rdev = dev;

  mutex_lock(&devfs_lock);
  list_add_tail(&entry->list, &devfs_entries);
  mutex_unlock(&devfs_lock);

  return 0;
}

EXPORT_SYMBOL(devfs_register_device);
