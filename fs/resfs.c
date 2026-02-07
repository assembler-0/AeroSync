/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/resfs.c
 * @brief Resource Domain Filesystem
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

#include <aerosync/errno.h>
#include <aerosync/types.h>
#include <aerosync/resdomain.h>
#include <fs/pseudo_fs.h>
#include <lib/printk.h>
#include <lib/uaccess.h>
#include <lib/vsprintf.h>

static struct pseudo_fs_info resfs_info = {
  .name = "resfs",
};

/* --- Control File Operations --- */

static ssize_t resfs_weight_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->private_data;
  if (*ppos > 0) return 0;

  char kbuf[32];
  size_t len = snprintf(kbuf, 32, "%u\n", rd->cpu_weight);
  if (count < len) len = count;

  if (copy_to_user(buf, kbuf, len) != 0) return -EFAULT;
  *ppos += len;
  return len;
}

static struct file_operations resfs_weight_fops = {
  .read = resfs_weight_read,
};

/**
 * resfs_init - Register resfs and mount it
 */
void resfs_init(void) {
  if (pseudo_fs_register(&resfs_info) != 0) {
    printk(KERN_ERR RESFS_CLASS "Failed to register filesystem\n");
    return;
  }

#ifdef CONFIG_RESFS_MOUNT
  /* Auto-mount at configured path */
  printk(KERN_INFO RESFS_CLASS "Automounting at %s\n", STRINGIFY(CONFIG_RESFS_MOUNT_PATH));
  vfs_mount(nullptr, STRINGIFY(CONFIG_RESFS_MOUNT_PATH), "resfs", 0, nullptr);
#endif
}

/**
 * resfs_bind_domain - Create directory structure for a new domain
 */
void resfs_bind_domain(struct resdomain *rd) {
  struct pseudo_node *parent_node = rd->parent ? rd->parent->private_data : nullptr;

  struct pseudo_node *dir = pseudo_fs_create_node(&resfs_info, parent_node, rd->name, S_IFDIR | 0755, nullptr, rd);
  rd->private_data = dir;

  /* Create control files */
  pseudo_fs_create_node(&resfs_info, dir, "cpu.weight", S_IFREG | 0644, &resfs_weight_fops, rd);
}
