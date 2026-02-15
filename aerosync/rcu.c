///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/rcu.c
 * @brief Tree-based Read-Copy-Update (RCU) implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/rcu.h>
#include <aerosync/srcu.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sched/process.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>
#include <aerosync/panic.h>
#include <aerosync/export.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <aerosync/classes.h>
#include <mm/slub.h>
#include <lib/string.h>

struct rcu_state rcu_state;
DEFINE_PER_CPU(struct rcu_data, rcu_data);

static int rcu_num_lvls;
static int rcu_num_nodes;
static int rcu_lvl_cnt[4];

/* Forward declarations */
static void rcu_process_callbacks(void);

/**
 * rcu_init_node_hierarchy - Build the RCU node tree
 */
static void rcu_init_node_hierarchy(void) {
  int i, j;
  int n = MAX_CPUS;
  int fanout = CONFIG_RCU_FANOUT;

  /* Calculate tree shape */
  rcu_num_lvls = 0;
  do {
    rcu_lvl_cnt[rcu_num_lvls] = (n + fanout - 1) / fanout;
    n = rcu_lvl_cnt[rcu_num_lvls];
    rcu_num_lvls++;
  } while (n > 1 && rcu_num_lvls < 4);

  rcu_num_nodes = 0;
  for (i = 0; i < rcu_num_lvls; i++) {
    rcu_state.level_offsets[i] = rcu_num_nodes;
    rcu_num_nodes += rcu_lvl_cnt[i];
  }

  rcu_state.nodes = kzalloc(sizeof(struct rcu_node) * rcu_num_nodes);
  if (!rcu_state.nodes) {
    panic("Failed to allocate RCU nodes");
  }

  /* Initialize nodes and link to parents */
  for (i = 0; i < rcu_num_nodes; i++) {
    struct rcu_node *rnp = &rcu_state.nodes[i];
    spinlock_init(&rnp->lock);
    cpumask_clear(&rnp->qs_mask);
    rnp->gp_seq = 0;
    rnp->completed_seq = 0;
  }

  for (i = 0; i < rcu_num_lvls; i++) {
    int level_start = rcu_state.level_offsets[i];
    int n_at_lvl = rcu_lvl_cnt[i];

    for (j = 0; j < n_at_lvl; j++) {
      struct rcu_node *rnp = &rcu_state.nodes[level_start + j];
      rnp->level = i;
      if (i == 0) {
        rnp->grp_start = j * fanout;
        rnp->grp_last = (j + 1) * fanout - 1;
        if (rnp->grp_last >= MAX_CPUS) rnp->grp_last = MAX_CPUS - 1;
      } else {
        rnp->grp_start = j * fanout;
        rnp->grp_last = (j + 1) * fanout - 1;
      }

      if (i < rcu_num_lvls - 1) {
        int next_level_start = rcu_state.level_offsets[i + 1];
        rnp->parent = &rcu_state.nodes[next_level_start + (j / fanout)];
      } else {
        rnp->parent = nullptr;
      }
    }
  }

  printk(KERN_INFO SYNC_CLASS "RCU Tree: %d levels, %d nodes, fanout %d\n",
         rcu_num_lvls, rcu_num_nodes, fanout);
}

void rcu_check_callbacks(void) {
  if (preemptible()) {
    rcu_qs();
  }
  rcu_process_callbacks();
}

static int rcu_cpu_kthread(void *data) {
  struct rcu_data *rdp = (struct rcu_data *) data;
  printk(KERN_DEBUG SYNC_CLASS "RCU kthread started for CPU %d\n", rdp->cpu);

  while (1) {
    /* Wait until there are callbacks to process or a GP ended */
    wait_event(rcu_state.gp_wait, rdp->callbacks || rdp->wait_callbacks ||
               rcu_state.completed_seq >= rdp->gp_seq);

    rcu_check_callbacks();
  }
  return 0;
}

void rcu_init(void) {
  rcu_init_node_hierarchy();
  init_waitqueue_head(&rcu_state.gp_wait);
  spinlock_init(&rcu_state.gp_lock);

  int cpu;
  for_each_possible_cpu(cpu) {
    struct rcu_data *rdp = per_cpu_ptr(rcu_data, cpu);
    memset(rdp, 0, sizeof(*rdp));
    rdp->cpu = cpu;
    rdp->callbacks_tail = &rdp->callbacks;
    rdp->wait_tail = &rdp->wait_callbacks;

    /* Point to leaf node */
    int leaf_idx = cpu / CONFIG_RCU_FANOUT;
    if (leaf_idx >= rcu_lvl_cnt[0]) leaf_idx = rcu_lvl_cnt[0] - 1;
    rdp->mynode = &rcu_state.nodes[leaf_idx];
  }

  printk(KERN_INFO SYNC_CLASS "Tree RCU Initialized (early)\n");
}

void rcu_spawn_kthreads(void) {
  int cpu;
  /* Only spawn for online CPUs for now to avoid overloading the scheduler
   * during boot with 512 threads if they are not all needed.
   */
  for_each_online_cpu(cpu) {
    struct rcu_data *rdp = per_cpu_ptr(rcu_data, cpu);
    char name[16];
    snprintf(name, sizeof(name), "rcu/%d", cpu);
    rdp->rcu_kthread = kthread_create(rcu_cpu_kthread, rdp, name);
    if (rdp->rcu_kthread) {
      kthread_run(rdp->rcu_kthread);
    }
  }
  printk(KERN_INFO SYNC_CLASS "RCU kthreads spawned for online CPUs\n");
}

/**
 * rcu_report_qs_rnp - Report a quiescent state to a node and propagate up
 */
static void rcu_report_qs_rnp(uint64_t mask, struct rcu_node *rnp, unsigned long gp_seq) {
  irq_flags_t flags;
  struct rcu_node *rnp_parent;

  for (;;) {
    flags = spinlock_lock_irqsave(&rnp->lock);

    /* If this GP already ended or we are reporting for wrong GP, abort */
    if (rnp->gp_seq != gp_seq || rnp->completed_seq == gp_seq) {
      spinlock_unlock_irqrestore(&rnp->lock, flags);
      return;
    }

    /* Clear our bit(s) in the mask.
     * For leaf nodes (level 0), qs_mask is a full cpumask of CPUs.
     * For internal nodes, qs_mask is a mask of children.
     */
    if (rnp->level == 0) {
      /* mask is a bit within the range [grp_start, grp_last] */
      /* We need to find which CPU this mask bit corresponds to */
      int bit = __builtin_ctzll(mask);
      int cpu = rnp->grp_start + bit;
      cpumask_clear_cpu(cpu, &rnp->qs_mask);
    } else {
      /* mask is a bitmask of children nodes */
      rnp->qs_mask.bits[0] &= ~mask;
    }

    if (!cpumask_empty(&rnp->qs_mask)) {
      /* Still waiting for others at this level */
      spinlock_unlock_irqrestore(&rnp->lock, flags);
      return;
    }

    /* This node finished! */
    rnp->completed_seq = gp_seq;
    rnp_parent = rnp->parent;

    if (!rnp_parent) {
      /* We reached the root! GP finished. */
      rcu_state.completed_seq = gp_seq;
      spinlock_unlock_irqrestore(&rnp->lock, flags);
      wake_up_all(&rcu_state.gp_wait);
      return;
    }

    /* Calculate mask for parent based on our index in our level */
    int level_idx = rnp - &rcu_state.nodes[rcu_state.level_offsets[rnp->level]];
    uint64_t parent_mask = (1ULL << (level_idx % CONFIG_RCU_FANOUT));

    spinlock_unlock_irqrestore(&rnp->lock, flags);

    /* Propagate up to parent */
    mask = parent_mask;
    rnp = rnp_parent;
    gp_seq = rnp->gp_seq;
  }
}

/**
 * rcu_qs - Report a quiescent state for the current CPU
 */
void rcu_qs(void) {
  struct rcu_data *rdp = this_cpu_ptr(rcu_data);
  struct rcu_node *rnp = rdp->mynode;

  if (!rdp->qs_pending) return;

  irq_flags_t flags = spinlock_lock_irqsave(&rnp->lock);
  if (rdp->qs_pending && rdp->gp_seq == rnp->gp_seq) {
    rdp->qs_pending = false;
    uint64_t mask = (1ULL << (rdp->cpu % CONFIG_RCU_FANOUT));
    spinlock_unlock_irqrestore(&rnp->lock, flags);

    rcu_report_qs_rnp(mask, rnp, rdp->gp_seq);
  } else {
    spinlock_unlock_irqrestore(&rnp->lock, flags);
  }
}

static void rcu_start_gp(void) {
  if (rcu_state.gp_seq != rcu_state.completed_seq)
    return;

  rcu_state.gp_seq++;

  /* Initialize tree for new GP */
  for (int i = 0; i < rcu_num_nodes; i++) {
    struct rcu_node *rnp = &rcu_state.nodes[i];
    spinlock_lock(&rnp->lock);
    rnp->gp_seq = rcu_state.gp_seq;
    rnp->completed_seq = rcu_state.gp_seq - 1;

    if (rnp->level == 0) {
      /* Leaf node: set mask for online CPUs in its range */
      cpumask_clear(&rnp->qs_mask);
      for (int cpu = rnp->grp_start; cpu <= rnp->grp_last; cpu++) {
        if (cpumask_test_cpu(cpu, &cpu_online_mask))
          cpumask_set_cpu(cpu, &rnp->qs_mask);
      }
    } else {
      /* Internal node: set mask for children */
      int level_idx = rnp - &rcu_state.nodes[rcu_state.level_offsets[rnp->level]];
      int n_children;
      int total_children = rcu_lvl_cnt[rnp->level - 1];

      if (level_idx == rcu_lvl_cnt[rnp->level] - 1) {
        /* Last node in this level might have fewer children */
        n_children = total_children % CONFIG_RCU_FANOUT;
        if (n_children == 0) n_children = CONFIG_RCU_FANOUT;
      } else {
        n_children = CONFIG_RCU_FANOUT;
      }

      cpumask_clear(&rnp->qs_mask);
      if (n_children == 64) {
        rnp->qs_mask.bits[0] = ~0ULL;
      } else {
        rnp->qs_mask.bits[0] = (1ULL << n_children) - 1;
      }
    }
    spinlock_unlock(&rnp->lock);
  }

  /* Update rdp for each CPU */
  int cpu;
  for_each_online_cpu(cpu) {
    struct rcu_data *rdp = per_cpu_ptr(rcu_data, cpu);
    rdp->gp_seq = rcu_state.gp_seq;
    rdp->qs_pending = true;
  }

  /* Wake up kthreads to start processing */
  wake_up_all(&rcu_state.gp_wait);
}

void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head)) {
  head->func = func;
  head->next = nullptr;

  irq_flags_t flags = local_irq_save();
  struct rcu_data *rdp = this_cpu_ptr(rcu_data);

  *rdp->callbacks_tail = head;
  rdp->callbacks_tail = &head->next;

  local_irq_restore(flags);
}

static void __no_cfi rcu_process_callbacks(void) {
  struct rcu_head *list = nullptr;
  struct rcu_data *rdp = this_cpu_ptr(rcu_data);

  /* 1. If current wait list finished its GP, move to local list for execution */
  if (rdp->wait_callbacks && rcu_state.completed_seq >= rdp->gp_seq) {
    list = rdp->wait_callbacks;
    rdp->wait_callbacks = nullptr;
    rdp->wait_tail = &rdp->wait_callbacks;
  }

  /* 2. If no wait list pending, move new callbacks to wait list and start GP */
  if (!rdp->wait_callbacks && rdp->callbacks) {
    irq_flags_t flags = spinlock_lock_irqsave(&rcu_state.gp_lock);

    rdp->wait_callbacks = rdp->callbacks;
    rdp->wait_tail = rdp->callbacks_tail;
    rdp->callbacks = nullptr;
    rdp->callbacks_tail = &rdp->callbacks;

    rcu_start_gp();
    rdp->gp_seq = rcu_state.gp_seq;
    rdp->qs_pending = true;

    spinlock_unlock_irqrestore(&rcu_state.gp_lock, flags);
  }

  /* 3. Execute ready callbacks */
  while (list) {
    struct rcu_head *next = list->next;
    list->func(list);
    list = next;
  }
}

void synchronize_rcu(void) {
  unsigned long wait_gp;

  irq_flags_t flags = spinlock_lock_irqsave(&rcu_state.gp_lock);
  rcu_start_gp();
  wait_gp = rcu_state.gp_seq;
  spinlock_unlock_irqrestore(&rcu_state.gp_lock, flags);

  rcu_qs();
  wait_event(rcu_state.gp_wait, rcu_state.completed_seq >= wait_gp);
}

void synchronize_rcu_expedited(void) {
  /* For now, just an alias for synchronize_rcu, but technically 
   * it should avoid the waitqueue and use IPIs for faster QS reporting.
   */
  synchronize_rcu();
}

struct rcu_test_data {
  struct rcu_head rcu;
  int done;
};

static void rcu_test_callback(struct rcu_head *head) {
  struct rcu_test_data *td = container_of(head, struct rcu_test_data, rcu);
  td->done = 1;
}

void rcu_test(void) {
  struct rcu_test_data data;
  data.done = 0;

  printk(KERN_INFO TEST_CLASS "Starting RCU smoke test...\n");

  call_rcu(&data.rcu, rcu_test_callback);
  synchronize_rcu();

  /* Wait a bit for callback if needed, but synchronize_rcu should be enough
     if we have a single GP. Actually, call_rcu might need another GP. */
  int timeout = 1000000;
  while (!data.done && timeout--) {
    rcu_check_callbacks();
    cpu_relax();
  }

  if (data.done) {
    printk(KERN_INFO TEST_CLASS "RCU smoke test passed!\n");
  } else {
    printk(KERN_ERR TEST_CLASS "RCU smoke test FAILED (callback not called)\n");
  }

  /* SRCU Test */
  struct srcu_struct ss;
  init_srcu_struct(&ss);
  printk(KERN_INFO TEST_CLASS "Starting SRCU smoke test...\n");
  int idx = srcu_read_lock(&ss);
  srcu_read_unlock(&ss, idx);
  synchronize_srcu(&ss);
  printk(KERN_INFO TEST_CLASS "SRCU smoke test passed!\n");
}

EXPORT_SYMBOL(call_rcu);
EXPORT_SYMBOL(synchronize_rcu);
EXPORT_SYMBOL(synchronize_rcu_expedited);
EXPORT_SYMBOL(rcu_qs);
