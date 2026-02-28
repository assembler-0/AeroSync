/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/devtmpfs.c
 * @brief Device Temporary Filesystem (based on tmpfs)
 * @copyright (C) 2026 assembler-0
 */

#ifdef CONFIG_DEVTMPFS

#include <fs/vfs.h>
#include <fs/devtmpfs.h>
#include <fs/tmpfs.h>
#include <aerosync/errno.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <aerosync/mutex.h>

static struct super_block *devtmpfs_sb = nullptr;

static int devtmpfs_mount(struct file_system_type *fs_type, const char *dev_name, const char *dir_name,
                       unsigned long flags, void *data) {
  (void) dev_name; (void) dir_name; (void) flags; (void) fs_type;
  struct super_block *sb = kzalloc(sizeof(struct super_block));
  if (!sb) return -ENOMEM;

  int ret = tmpfs_fill_super(sb, data);
  if (ret) {
    kfree(sb);
    return ret;
  }

  extern struct list_head super_blocks;
  extern struct mutex sb_mutex;
  mutex_lock(&sb_mutex);
  list_add_tail(&sb->sb_list, &super_blocks);
  mutex_unlock(&sb_mutex);

  devtmpfs_sb = sb;
  return 0;
}

static void devtmpfs_kill_sb(struct super_block *sb) {
  if (devtmpfs_sb == sb) devtmpfs_sb = nullptr;
  tmpfs_type.kill_sb(sb);
}

static struct file_system_type devtmpfs_type = {
  .name = "devtmpfs",
  .mount = devtmpfs_mount,
  .kill_sb = devtmpfs_kill_sb,
};

int devtmpfs_register_device(const char *name, const char *category, vfs_mode_t mode, dev_t dev) {
  if (!devtmpfs_sb) return -ENODEV;

  struct tmpfs_node *parent = nullptr;
  if (category) {
      parent = tmpfs_mkdir_kern(devtmpfs_sb, nullptr, category, 0755);
      if (!parent) return -ENOMEM;
  }

  return tmpfs_create_kern(devtmpfs_sb, parent, name, mode, dev);
}

void devtmpfs_init(void) {
  register_filesystem(&devtmpfs_type);
}

#endif
