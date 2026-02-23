/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/sysfs.c
 * @brief System Filesystem (Kernel Object View)
 * @copyright (C) 2026 assembler-0
 */

#ifdef CONFIG_SYSFS

#include <fs/pseudo_fs.h>

static struct pseudo_fs_info sysfs_info = {
  .name = "sysfs",
};

void sysfs_init(void) {
  pseudo_fs_register(&sysfs_info);

  pseudo_fs_create_dir(&sysfs_info, nullptr, "kernel");
  pseudo_fs_create_dir(&sysfs_info, nullptr, "fs");
  pseudo_fs_create_dir(&sysfs_info, nullptr, "block");
  pseudo_fs_create_dir(&sysfs_info, nullptr, "bus");
  pseudo_fs_create_dir(&sysfs_info, nullptr, "class");
}

#endif
