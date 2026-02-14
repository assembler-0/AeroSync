/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/procfs.c
 * @brief Process Information Filesystem
 * @copyright (C) 2026 assembler-0
 */

#include <printk.h>
#include <aerosync/classes.h>
#include <fs/pseudo_fs.h>
#include <aerosync/sched/process.h>
#include <lib/vsprintf.h>
#include <aerosync/timer.h>
#include <mm/vm_object.h>
#include <arch/x86_64/mm/pmm.h>

static struct pseudo_fs_info procfs_info = {
  .name = "proc",
};

/* /proc/meminfo */
static ssize_t proc_meminfo_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[512];

  pmm_stats_t *stats = pmm_get_stats();
  
  unsigned long total = stats->total_pages;
  unsigned long free = stats->free_pages;

  int len = snprintf(kbuf, sizeof(kbuf),
                     "MemTotal:       %lu kB\n"
                     "MemFree:        %lu kB\n"
                     "MemAvailable:   %lu kB\n"
                     "ShadowObjects:  %ld\n",
                     total * 4, free * 4, free * 4,
                     atomic_long_read(&nr_shadow_objects));

  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations proc_meminfo_fops = {
  .read = proc_meminfo_read,
};

/* /proc/uptime */
static ssize_t proc_uptime_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[64];
  uint64_t now_ms = get_time_ns() / 1000000;
  int len = snprintf(kbuf, sizeof(kbuf), "%lu.%02lu 0.00\n",
                     (unsigned long) (now_ms / 1000),
                     (unsigned long) ((now_ms % 1000) / 10));
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations proc_uptime_fops = {
  .read = proc_uptime_read,
};

void procfs_init(void) {
  pseudo_fs_register(&procfs_info);

  pseudo_fs_create_file(&procfs_info, nullptr, "meminfo", &proc_meminfo_fops, nullptr);
  pseudo_fs_create_file(&procfs_info, nullptr, "uptime", &proc_uptime_fops, nullptr);
}
