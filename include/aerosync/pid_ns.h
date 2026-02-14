#pragma once

#include <aerosync/types.h>
#include <aerosync/atomic.h>
#include <aerosync/spinlock.h>
#include <aerosync/kref.h>
#include <linux/list.h>
#include <lib/id_alloc.h>

struct task_struct;

struct pid_namespace {
    struct kref kref;
    struct pid_namespace *parent;
    struct ida pid_ida;
    unsigned int level;
    struct task_struct *child_reaper;
};

extern struct pid_namespace init_pid_ns;

struct pid_namespace *create_pid_namespace(struct pid_namespace *parent);
void put_pid_ns(struct pid_namespace *ns);
pid_t pid_ns_alloc(struct pid_namespace *ns);
void pid_ns_free(struct pid_namespace *ns, pid_t pid);
pid_t task_pid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns);
