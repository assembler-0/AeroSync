/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/devtmpfs.c
 * @brief Device Temporary Filesystem
 * @copyright (C) 2026 assembler-0
 */

#ifdef CONFIG_DEVTMPFS

#include <fs/pseudo_fs.h>
#include <aerosync/errno.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <fs/vfs.h>
#include <fs/devtmpfs.h>

static struct pseudo_fs_info devtmpfs_info = {
  .name = "devtmpfs",
};

static void devtmpfs_init_inode(struct inode *inode, struct pseudo_node *pnode) {
  inode->i_mode = pnode->mode;
  inode->i_rdev = (dev_t) (uintptr_t) pnode->private_data;
  init_special_inode(inode, inode->i_mode, inode->i_rdev);
}

int devtmpfs_register_device(const char *name, vfs_mode_t mode, dev_t dev,
                          const struct file_operations *fops, void *private_data) {
  if (!devtmpfs_info.root) return -ENODEV;

  struct pseudo_node *node = pseudo_fs_create_node(&devtmpfs_info, nullptr, name, mode, fops, private_data);
  if (!node) return -ENOMEM;

  node->init_inode = devtmpfs_init_inode;
  /* Store dev_t in private_data for the callback */
  node->private_data = (void *) (uintptr_t) dev;

  return 0;
}

void devtmpfs_init(void) {
  pseudo_fs_register(&devtmpfs_info);
}

#endif
