/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vfs.c
 * @brief Memory Management VFS Integration
 * @copyright (C) 2026 assembler-0
 */

#include <mm/mm_types.h>
#include <fs/procfs.h>
#include <fs/sysfs.h>
#include <aerosync/resdomain.h>
#include <lib/string.h>
#include <arch/x86_64/mm/pmm.h>

/* --- sysfs: /runtime/sys/mm/ tuning --- */

static ssize_t mm_ksm_enabled_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[4];
  extern bool ksm_enabled;
  const int len = snprintf(kbuf, sizeof(kbuf), "%d", ksm_enabled);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations mm_ksm_fops = {
  .read = mm_ksm_enabled_read,
};

int mm_vfs_init(void) {
  /* sysfs tuning nodes */
  sysfs_create_dir_kern("ksm", "mm");
  sysfs_create_file_kern("enabled", &mm_ksm_fops, nullptr, "mm/ksm");

  sysfs_create_dir_kern("actl", "mm");
  return 0;
}
