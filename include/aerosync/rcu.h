/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/sched/cpumask.h>
#include <aerosync/wait.h>
#include <linux/rcupdate.h>

/**
 * struct rcu_node - Hierarchical RCU node
 */
alignas(64) struct rcu_node {
    spinlock_t lock;
    unsigned long gp_seq;      /* Grace period sequence number */
    unsigned long completed_seq;
    
    struct cpumask qs_mask;    /* CPUs/nodes that haven't reported QS yet */
    struct cpumask exp_mask;   /* CPUs/nodes that haven't reported QS for expedited GP */
    
    struct rcu_node *parent;
    int level;
    int grp_start;             /* First CPU/node in this group */
    int grp_last;              /* Last CPU/node in this group */
};

/**
 * struct rcu_state - Global RCU state
 */
struct rcu_state {
    struct rcu_node *nodes;    /* Array of rcu_nodes */
    int num_nodes;
    int num_levels;
    int level_offsets[4];      /* Offsets to levels in nodes array */
    
    spinlock_t gp_lock;
    unsigned long gp_seq;
    unsigned long completed_seq;
    
    wait_queue_head_t gp_wait;
    
    struct task_struct *gp_kthread;
};

/**
 * struct rcu_data - Per-CPU RCU state
 */
struct rcu_data {
    unsigned long gp_seq;      /* GP number this CPU is waiting for */
    bool qs_pending;           /* True if this CPU needs a quiescent state */
    
    struct rcu_node *mynode;   /* Leaf node for this CPU */
    int cpu;
    
    /* Callbacks */
    struct rcu_head *callbacks;
    struct rcu_head **callbacks_tail;
    
    struct rcu_head *wait_callbacks;
    struct rcu_head **wait_tail;
    
    struct task_struct *rcu_kthread;
};

extern struct rcu_state rcu_state;
DECLARE_PER_CPU(struct rcu_data, rcu_data);

void rcu_init(void);
void rcu_spawn_kthreads(void);
void rcu_check_callbacks(void);
void rcu_test(void);
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head));
void synchronize_rcu(void);
void rcu_barrier(void);
void rcu_qs(void);
