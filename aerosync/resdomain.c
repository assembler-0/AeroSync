/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/resdomain.c
 * @brief Advanced Resource Domain (ResDomain) Implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/resdomain.h>
#include <aerosync/sched/process.h>
#include <aerosync/errno.h>
#include <aerosync/timer.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <lib/uaccess.h>
#include <lib/vsprintf.h>
#include <fs/pseudo_fs.h>
#include <arch/x86_64/smp.h>
#include <linux/rbtree.h>

struct resdomain root_resdomain;

/* --- CPU Controller Ops --- */

static struct resdomain_subsys_state *cpu_css_alloc(struct resdomain *rd) {
  struct cpu_rd_state *cs = kzalloc(sizeof(struct cpu_rd_state));
  if (!cs) return nullptr;
  cs->weight = 1024;

  cs->se = kzalloc(sizeof(struct sched_entity *) * MAX_CPUS);
  cs->cfs_rq = kzalloc(sizeof(struct cfs_rq *) * MAX_CPUS);

  if (!cs->se || !cs->cfs_rq) goto fail;

  struct cpu_rd_state *parent_cs = nullptr;
  if (rd->parent) {
    parent_cs = (struct cpu_rd_state *) rd->parent->subsys[RD_SUBSYS_CPU];
  }

  for (int i = 0; i < MAX_CPUS; i++) {
    cs->cfs_rq[i] = kzalloc(sizeof(struct cfs_rq));
    if (!cs->cfs_rq[i]) goto fail;

    /* Initialize CFS RQ */
    cs->cfs_rq[i]->tasks_timeline = RB_ROOT;
    cs->cfs_rq[i]->rb_leftmost = nullptr;

    /* Root domain doesn't need its own sched_entities, it uses the RQ's root */
    if (rd == &root_resdomain) {
      struct rq *rq = per_cpu_ptr(runqueues, i);
      /* Redirect root's cfs_rq to the actual per-cpu runqueue */
      kfree(cs->cfs_rq[i]);
      cs->cfs_rq[i] = &rq->cfs;
      cs->se[i] = nullptr;
      continue;
    }

    cs->se[i] = kzalloc(sizeof(struct sched_entity));
    if (!cs->se[i]) goto fail;

    /* Initialize SE */
    cs->se[i]->my_q = cs->cfs_rq[i];
    cs->se[i]->load.weight = cs->weight;
    cs->se[i]->exec_start_ns = get_time_ns();

    /* Link to parent */
    struct rq *rq = per_cpu_ptr(runqueues, i);
    cs->se[i]->cfs_rq = parent_cs ? parent_cs->cfs_rq[i] : &rq->cfs;
    cs->se[i]->parent = parent_cs ? parent_cs->se[i] : nullptr;
  }

  return &cs->css;

fail:
  if (cs->se) {
    for (int i = 0; i < MAX_CPUS; i++) if (cs->se[i]) kfree(cs->se[i]);
    kfree(cs->se);
  }
  if (cs->cfs_rq) {
    for (int i = 0; i < MAX_CPUS; i++) {
      /* Don't free root's RQs which are static */
      if (rd == &root_resdomain) break;
      if (cs->cfs_rq[i]) kfree(cs->cfs_rq[i]);
    }
    kfree(cs->cfs_rq);
  }
  kfree(cs);
  return nullptr;
}

static void cpu_css_free(struct resdomain *rd) {
  struct cpu_rd_state *cs = (struct cpu_rd_state *) rd->subsys[RD_SUBSYS_CPU];
  if (!cs) return;

  if (cs->se) {
    for (int i = 0; i < MAX_CPUS; i++) {
      if (cs->se[i]) kfree(cs->se[i]);
    }
    kfree(cs->se);
  }

  if (cs->cfs_rq) {
    for (int i = 0; i < MAX_CPUS; i++) {
      /* Never free the root runqueues */
      if (rd == &root_resdomain) break;
      if (cs->cfs_rq[i]) kfree(cs->cfs_rq[i]);
    }
    kfree(cs->cfs_rq);
  }

  kfree(cs);
}

static ssize_t resfs_cpu_weight_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct cpu_rd_state *cs = (struct cpu_rd_state *) rd->subsys[RD_SUBSYS_CPU];
  char kbuf[32];
  int len = snprintf(kbuf, sizeof(kbuf), "%u\n", cs->weight);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t resfs_cpu_weight_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct cpu_rd_state *cs = (struct cpu_rd_state *) rd->subsys[RD_SUBSYS_CPU];
  char kbuf[32];
  if (count >= sizeof(kbuf)) return -EINVAL;
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  unsigned long val;
  if (kstrtoul(kbuf, 10, &val)) return -EINVAL;
  cs->weight = (uint32_t) val;
  return (ssize_t) count;
}

static const struct file_operations resfs_cpu_weight_fops = {
  .read = resfs_cpu_weight_read,
  .write = resfs_cpu_weight_write,
};

static void cpu_populate(struct resdomain *rd, struct pseudo_node *dir) {
  extern struct pseudo_fs_info resfs_info;
  extern void resfs_init_inode(struct inode *inode, struct pseudo_node *pnode);
  struct pseudo_node *node;
  node = pseudo_fs_create_file(&resfs_info, dir, "cpu.weight", &resfs_cpu_weight_fops, rd);
  if (node) node->init_inode = resfs_init_inode;
}

static struct rd_subsys cpu_subsys = {
  .name = "cpu",
  .id = RD_SUBSYS_CPU,
  .css_alloc = cpu_css_alloc,
  .css_free = cpu_css_free,
  .populate = cpu_populate,
};

/* --- Memory Controller Ops --- */

static ssize_t resfs_mem_max_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct mem_rd_state *ms = (struct mem_rd_state *) rd->subsys[RD_SUBSYS_MEM];
  char kbuf[32];
  int len;
  if (ms->max == (uint64_t) -1)
    len = snprintf(kbuf, sizeof(kbuf), "max\n");
  else
    len = snprintf(kbuf, sizeof(kbuf), "%llu\n", (unsigned long long) ms->max);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t resfs_mem_max_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct mem_rd_state *ms = (struct mem_rd_state *) rd->subsys[RD_SUBSYS_MEM];
  char kbuf[32];
  if (count >= sizeof(kbuf)) return -EINVAL;
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  unsigned long long val;
  if (strncmp(kbuf, "max", 3) == 0) {
    val = (uint64_t) -1;
  } else {
    if (kstrtoull(kbuf, 10, &val)) return -EINVAL;
  }
  ms->max = (uint64_t) val;
  return (ssize_t) count;
}

static const struct file_operations resfs_mem_max_fops = {
  .read = resfs_mem_max_read,
  .write = resfs_mem_max_write,
};

static ssize_t resfs_mem_current_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct mem_rd_state *ms = (struct mem_rd_state *) rd->subsys[RD_SUBSYS_MEM];
  char kbuf[32];
  int len = snprintf(kbuf, sizeof(kbuf), "%llu\n", (unsigned long long) atomic64_read(&ms->usage));
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations resfs_mem_current_fops = {
  .read = resfs_mem_current_read,
};

static void mem_populate(struct resdomain *rd, struct pseudo_node *dir) {
  extern struct pseudo_fs_info resfs_info;
  extern void resfs_init_inode(struct inode *inode, struct pseudo_node *pnode);
  struct pseudo_node *node;
  node = pseudo_fs_create_file(&resfs_info, dir, "memory.max", &resfs_mem_max_fops, rd);
  if (node) node->init_inode = resfs_init_inode;
  node = pseudo_fs_create_file(&resfs_info, dir, "memory.current", &resfs_mem_current_fops, rd);
  if (node) node->init_inode = resfs_init_inode;
}

static struct resdomain_subsys_state *mem_css_alloc(struct resdomain *rd) {
  struct mem_rd_state *ms = kzalloc(sizeof(struct mem_rd_state));
  if (!ms) return nullptr;
  ms->max = (uint64_t) -1;
  ms->high = (uint64_t) -1;
  return &ms->css;
}

static void mem_css_free(struct resdomain *rd) {
  kfree(rd->subsys[RD_SUBSYS_MEM]);
}

static struct rd_subsys mem_subsys = {
  .name = "memory",
  .id = RD_SUBSYS_MEM,
  .css_alloc = mem_css_alloc,
  .css_free = mem_css_free,
  .populate = mem_populate,
};

/* --- PID Controller Ops --- */

static ssize_t resfs_pid_max_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct pid_rd_state *ps = (struct pid_rd_state *) rd->subsys[RD_SUBSYS_PID];
  char kbuf[32];
  int len;
  if (ps->max == -1)
    len = snprintf(kbuf, sizeof(kbuf), "max\n");
  else
    len = snprintf(kbuf, sizeof(kbuf), "%d\n", ps->max);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t resfs_pid_max_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct pid_rd_state *ps = (struct pid_rd_state *) rd->subsys[RD_SUBSYS_PID];
  char kbuf[32];
  if (count >= sizeof(kbuf)) return -EINVAL;
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  int val;
  if (strncmp(kbuf, "max", 3) == 0) {
    val = -1;
  } else {
    if (kstrtoint(kbuf, 10, &val)) return -EINVAL;
  }
  ps->max = val;
  return (ssize_t) count;
}

static const struct file_operations resfs_pid_max_fops = {
  .read = resfs_pid_max_read,
  .write = resfs_pid_max_write,
};

static ssize_t resfs_pid_current_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct pid_rd_state *ps = (struct pid_rd_state *) rd->subsys[RD_SUBSYS_PID];
  char kbuf[32];
  int len = snprintf(kbuf, sizeof(kbuf), "%d\n", atomic_read(&ps->count));
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations resfs_pid_current_fops = {
  .read = resfs_pid_current_read,
};

static void pid_populate(struct resdomain *rd, struct pseudo_node *dir) {
  extern struct pseudo_fs_info resfs_info;
  extern void resfs_init_inode(struct inode *inode, struct pseudo_node *pnode);
  struct pseudo_node *node;
  node = pseudo_fs_create_file(&resfs_info, dir, "pids.max", &resfs_pid_max_fops, rd);
  if (node) node->init_inode = resfs_init_inode;
  node = pseudo_fs_create_file(&resfs_info, dir, "pids.current", &resfs_pid_current_fops, rd);
  if (node) node->init_inode = resfs_init_inode;
}

static struct resdomain_subsys_state *pid_css_alloc(struct resdomain *rd) {
  struct pid_rd_state *ps = kzalloc(sizeof(struct pid_rd_state));
  if (!ps) return nullptr;
  ps->max = -1;
  return &ps->css;
}

static void pid_css_free(struct resdomain *rd) {
  kfree(rd->subsys[RD_SUBSYS_PID]);
}

static struct rd_subsys pid_subsys = {
  .name = "pids",
  .id = RD_SUBSYS_PID,
  .css_alloc = pid_css_alloc,
  .css_free = pid_css_free,
  .populate = pid_populate,
};

/* --- IO Controller Ops --- */

struct io_rd_state {
  struct resdomain_subsys_state css;
  uint32_t weight; /* I/O Weight (10-1000, default 100) */
  uint64_t max_bps; /* Max Bytes Per Second (0 = unlimited) */
  uint64_t max_iops; /* Max IOPS (0 = unlimited) */

  /* Token Bucket State */
  spinlock_t lock;
  uint64_t bps_tokens;
  uint64_t iops_tokens;
  uint64_t last_refill_ns;
};

static struct resdomain_subsys_state *io_css_alloc(struct resdomain *rd) {
  struct io_rd_state *ios = kzalloc(sizeof(struct io_rd_state));
  if (!ios) return nullptr;
  ios->weight = 100;
  ios->max_bps = 0;
  ios->max_iops = 0;
  spinlock_init(&ios->lock);
  ios->last_refill_ns = get_time_ns();
  ios->bps_tokens = 0;
  ios->iops_tokens = 0;
  return &ios->css;
}

static void io_css_free(struct resdomain *rd) {
  kfree(rd->subsys[RD_SUBSYS_IO]);
}

static ssize_t resfs_io_weight_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct io_rd_state *ios = (struct io_rd_state *) rd->subsys[RD_SUBSYS_IO];
  char kbuf[32];
  int len = snprintf(kbuf, sizeof(kbuf), "%u\n", ios->weight);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t resfs_io_weight_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct resdomain *rd = file->f_inode->i_fs_info;
  struct io_rd_state *ios = (struct io_rd_state *) rd->subsys[RD_SUBSYS_IO];
  char kbuf[32];
  if (count >= sizeof(kbuf)) return -EINVAL;
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  unsigned long val;
  if (kstrtoul(kbuf, 10, &val)) return -EINVAL;
  if (val < 10 || val > 1000) return -EINVAL;
  ios->weight = (uint32_t) val;
  return (ssize_t) count;
}

static const struct file_operations resfs_io_weight_fops = {
  .read = resfs_io_weight_read,
  .write = resfs_io_weight_write,
};

static void io_populate(struct resdomain *rd, struct pseudo_node *dir) {
  extern struct pseudo_fs_info resfs_info;
  extern void resfs_init_inode(struct inode *inode, struct pseudo_node *pnode);
  struct pseudo_node *node;
  node = pseudo_fs_create_file(&resfs_info, dir, "io.weight", &resfs_io_weight_fops, rd);
  if (node) node->init_inode = resfs_init_inode;
}

static struct rd_subsys io_subsys = {
  .name = "io",
  .id = RD_SUBSYS_IO,
  .css_alloc = io_css_alloc,
  .css_free = io_css_free,
  .populate = io_populate,
};

struct rd_subsys *rd_subsys_list[RD_SUBSYS_COUNT] = {
  [RD_SUBSYS_CPU] = &cpu_subsys,
  [RD_SUBSYS_MEM] = &mem_subsys,
  [RD_SUBSYS_PID] = &pid_subsys,
  [RD_SUBSYS_IO] = &io_subsys,
};

/* --- Core Management --- */

static int __no_cfi resdomain_init_subsys(struct resdomain *rd, int subsys_id) {
  struct rd_subsys *ss = rd_subsys_list[subsys_id];
  if (!ss || !ss->css_alloc) return 0;
  struct resdomain_subsys_state *css = ss->css_alloc(rd);
  if (!css) return -ENOMEM;
  css->rd = rd;
  rd->subsys[subsys_id] = css;
  return 0;
}

void resdomain_init(void) {
  memset(&root_resdomain, 0, sizeof(root_resdomain));
  strncpy(root_resdomain.name, "root", sizeof(root_resdomain.name));
  atomic_set(&root_resdomain.refcount, 1);
  INIT_LIST_HEAD(&root_resdomain.children);
  INIT_LIST_HEAD(&root_resdomain.sibling);
  spinlock_init(&root_resdomain.lock);
  root_resdomain.subtree_control = (1 << RD_SUBSYS_CPU) | (1 << RD_SUBSYS_MEM) | (1 << RD_SUBSYS_PID) | (
                                     1 << RD_SUBSYS_IO);
  root_resdomain.child_subsys_mask = root_resdomain.subtree_control;
  for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
    resdomain_init_subsys(&root_resdomain, i);
  }
  resfs_init();
  printk(KERN_INFO SCHED_CLASS "Advanced Resource Domains (ResDomain) v2 initialized\n");
}

struct resdomain *resdomain_create(struct resdomain *parent, const char *name) {
  struct resdomain *rd = kzalloc(sizeof(struct resdomain));
  if (!rd) return nullptr;
  strncpy(rd->name, name, sizeof(rd->name));
  atomic_set(&rd->refcount, 1);
  INIT_LIST_HEAD(&rd->children);
  INIT_LIST_HEAD(&rd->sibling);
  spinlock_init(&rd->lock);
  rd->parent = parent;
  if (parent) {
    uint32_t mask = parent->subtree_control;
    rd->child_subsys_mask = mask;
    for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
      if (mask & (1 << i)) {
        resdomain_init_subsys(rd, i);
      }
    }
    resdomain_get(parent);
    spinlock_lock(&parent->lock);
    list_add_tail(&rd->sibling, &parent->children);
    spinlock_unlock(&parent->lock);
  }
  resfs_bind_domain(rd);
  return rd;
}

void __no_cfi resdomain_put(struct resdomain *rd) {
  if (!rd || rd == &root_resdomain) return;
  if (atomic_dec_and_test(&rd->refcount)) {
    if (rd->parent) {
      spinlock_lock(&rd->parent->lock);
      list_del(&rd->sibling);
      spinlock_unlock(&rd->parent->lock);
      resdomain_put(rd->parent);
    }
    for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
      if (rd->subsys[i] && rd_subsys_list[i]->css_free) {
        rd_subsys_list[i]->css_free(rd);
      }
    }
    kfree(rd);
  }
}

void resdomain_task_init(struct task_struct *p, struct task_struct *parent) {
  if (parent && parent->rd) {
    p->rd = parent->rd;
  } else {
    p->rd = &root_resdomain;
  }
  resdomain_get(p->rd);
  struct pid_rd_state *ps = (struct pid_rd_state *) p->rd->subsys[RD_SUBSYS_PID];
  if (ps) {
    atomic_inc(&ps->count);
  }
}

void resdomain_task_exit(struct task_struct *p) {
  if (!p->rd) return;
  struct pid_rd_state *ps = (struct pid_rd_state *) p->rd->subsys[RD_SUBSYS_PID];
  if (ps) {
    atomic_dec(&ps->count);
  }
  resdomain_put(p->rd);
  p->rd = nullptr;
}

int __no_cfi resdomain_attach_task(struct resdomain *rd, struct task_struct *task) {
  if (!rd || !task) return -EINVAL;
  struct resdomain *old_rd = task->rd;
  if (rd == old_rd) return 0;
  for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
    if (rd->subsys[i] && rd_subsys_list[i]->can_attach) {
      int ret = rd_subsys_list[i]->can_attach(rd, task);
      if (ret < 0) return ret;
    }
  }
  resdomain_get(rd);
  task->rd = rd;
  for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
    if (rd->subsys[i] && rd_subsys_list[i]->attach) {
      rd_subsys_list[i]->attach(rd, task);
    }
  }
  if (old_rd) {
    struct pid_rd_state *ops = (struct pid_rd_state *) old_rd->subsys[RD_SUBSYS_PID];
    struct pid_rd_state *nps = (struct pid_rd_state *) rd->subsys[RD_SUBSYS_PID];
    if (ops) atomic_dec(&ops->count);
    if (nps) atomic_inc(&nps->count);
    resdomain_put(old_rd);
  }
  sched_move_task(task);
  return 0;
}

int __no_cfi resdomain_charge_mem(struct resdomain *rd, uint64_t bytes, bool force) {
  if (!rd) rd = &root_resdomain;
  struct mem_rd_state *ms = (struct mem_rd_state *) rd->subsys[RD_SUBSYS_MEM];
  if (!ms) return 0;

  uint64_t usage = atomic64_read(&ms->usage);
  
  /* 
   * HARD LIMIT ENFORCEMENT:
   * If we exceed the limit, try direct reclaim before failing.
   */
  if (!force && (ms->max != (uint64_t) -1) && (usage + bytes > ms->max)) {
#ifdef CONFIG_MM_RESDOMAIN_DIRECT_RECLAIM
    /* 
     * DIRECT RECLAIM:
     * We call into the memory management system to attempt to free pages.
     */
    extern size_t try_to_free_pages(struct pglist_data *pgdat, size_t nr_to_reclaim, gfp_t gfp_mask);
    
    size_t nr_to_reclaim = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    /* Try to reclaim from all online nodes */
    for (int i = 0; i < MAX_NUMNODES; i++) {
        if (node_data[i]) {
            try_to_free_pages(node_data[i], nr_to_reclaim, GFP_KERNEL);
        }
    }

    /* Re-check usage after reclaim */
    usage = atomic64_read(&ms->usage);
    if (usage + bytes > ms->max) {
        return -ENOMEM;
    }
#else
    return -ENOMEM;
#endif
  }

  atomic64_add(bytes, &ms->usage);
  if (rd->parent) {
    if (resdomain_charge_mem(rd->parent, bytes, force) < 0) {
      atomic64_sub(bytes, &ms->usage);
      return -ENOMEM;
    }
  }
  return 0;
}

void resdomain_uncharge_mem(struct resdomain *rd, uint64_t bytes) {
  if (!rd) rd = &root_resdomain;
  struct mem_rd_state *ms = (struct mem_rd_state *) rd->subsys[RD_SUBSYS_MEM];
  if (!ms) return;
  atomic64_sub(bytes, &ms->usage);
  if (rd->parent) {
    resdomain_uncharge_mem(rd->parent, bytes);
  }
}

int resdomain_can_fork(struct resdomain *rd) {
  if (!rd) rd = &root_resdomain;
  struct pid_rd_state *ps = (struct pid_rd_state *) rd->subsys[RD_SUBSYS_PID];
  if (ps && ps->max != -1) {
    if (atomic_read(&ps->count) >= ps->max) return -EAGAIN;
  }
  if (rd->parent) return resdomain_can_fork(rd->parent);
  return 0;
}

void resdomain_cancel_fork(struct resdomain *rd) {
  (void) rd;
}

bool resdomain_is_descendant(struct resdomain *parent, struct resdomain *child) {
  if (!parent || !child) return false;
  if (parent == child) return true;
  struct resdomain *tmp = child;
  while (tmp) {
    if (tmp == parent) return true;
    tmp = tmp->parent;
  }
  return false;
}

int resdomain_io_throttle(struct resdomain *rd, uint64_t bytes) {
  if (!rd) return 0;
  struct io_rd_state *ios = (struct io_rd_state *) rd->subsys[RD_SUBSYS_IO];
  if (!ios) return 0;

  /* Simple Token Bucket */
  /* If max_bps or max_iops are set, throttle */

  if (ios->max_bps == 0 && ios->max_iops == 0) return 0;

  /* Logic:
     1. Refill tokens based on time elapsed.
     2. Check if enough tokens.
     3. If not, sleep (throttle).
  */

  spinlock_lock(&ios->lock);
  uint64_t now = get_time_ns();
  uint64_t elapsed = now - ios->last_refill_ns;

  /* Refill BPS */
  if (ios->max_bps > 0) {
    uint64_t new_tokens = (elapsed * ios->max_bps) / 1000000000ULL;
    ios->bps_tokens += new_tokens;
    if (ios->bps_tokens > ios->max_bps) ios->bps_tokens = ios->max_bps; /* Max burst = 1 sec */
  }

  /* Refill IOPS */
  if (ios->max_iops > 0) {
    uint64_t new_tokens = (elapsed * ios->max_iops) / 1000000000ULL;
    ios->iops_tokens += new_tokens;
    if (ios->iops_tokens > ios->max_iops) ios->iops_tokens = ios->max_iops;
  }

  ios->last_refill_ns = now;

  bool wait = false;
  uint64_t wait_ns = 0;

  /* Check BPS */
  if (ios->max_bps > 0) {
    if (ios->bps_tokens < bytes) {
      wait = true;
      uint64_t needed = bytes - ios->bps_tokens;
      wait_ns = (needed * 1000000000ULL) / ios->max_bps;
    } else {
      ios->bps_tokens -= bytes;
    }
  }

  /* Check IOPS (1 IO) */
  if (ios->max_iops > 0) {
    if (ios->iops_tokens < 1) {
      wait = true;
      uint64_t needed = 1;
      uint64_t w = (needed * 1000000000ULL) / ios->max_iops;
      if (w > wait_ns) wait_ns = w;
    } else {
      ios->iops_tokens -= 1;
    }
  }

  spinlock_unlock(&ios->lock);

  if (wait) {
    /* Cap wait time to avoid excessively long sleeps */
    if (wait_ns > 100000000ULL) wait_ns = 100000000ULL; /* 100ms max sleep per check */

    /* Sleep */
    schedule_timeout(wait_ns);

    /* Retry */
    return resdomain_io_throttle(rd, bytes);
  }

  return 0;
}
