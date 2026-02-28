/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/vfs.c
 * @brief Scheduler-VFS Integration (procfs, sysfs, resfs)
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sched/sched.h>
#include <fs/procfs.h>
#include <fs/sysfs.h>
#include <fs/pseudo_fs.h>
#include <aerosync/resdomain.h>
#include <lib/string.h>
#include <aerosync/classes.h>
#include <arch/x86_64/smp.h>

#include "mm/slub.h"

/* --- procfs: /runtime/processes/sched_stats --- */

static ssize_t sched_stats_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char *kbuf = kmalloc(4096);
  if (!kbuf) return -ENOMEM;

  int len = 0;
  len += snprintf(kbuf + len, 4096 - len, "Scheduler Statistics:\n");

  for (int i = 0; i < smp_get_cpu_count(); i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    len += snprintf(kbuf + len, 4096 - len,
                    "  CPU %d: nr_running=%u, load_avg=%lu, util_avg=%lu, switches=%llu\n",
                    i, rq->nr_running, rq->cfs.avg.load_avg, rq->cfs.avg.util_avg,
                    rq->stats.nr_switches);
  }

  ssize_t ret = simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
  kfree(kbuf);
  return ret;
}

static const struct file_operations sched_stats_fops = {
  .read = sched_stats_read,
};

/* --- resfs: CPU Controller --- */

static ssize_t cpu_weight_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct pseudo_node *node = file->f_inode->i_fs_info;
  if (!node) return -EIO;
  struct resdomain *rd = node->private_data;
  if (!rd) return -EIO;

  struct cpu_rd_state *state = (struct cpu_rd_state *) rd->subsys[RD_SUBSYS_CPU];
  char kbuf[32];
  int len = snprintf(kbuf, sizeof(kbuf), "%u\n", state->weight);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations cpu_weight_fops = {
  .read = cpu_weight_read,
};

static void cpu_rd_populate(struct resdomain *rd, struct pseudo_node *dir) {
  /* resfs uses init_inode to set i_fs_info to rd, 
   * but our new pattern prefers i_fs_info = pnode.
   * However, resfs_init_inode still exists.
   */
}

int sched_vfs_init(void) {
  /* procfs integration */
  procfs_create_file_kern("sched_stats", &sched_stats_fops, nullptr);

  /* sysfs integration */
  sysfs_create_dir_kern("sched", "actl/sched");

  /* resfs integration is handled via the rd_subsys_list */
  return 0;
}
