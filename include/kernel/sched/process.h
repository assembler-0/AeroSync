#pragma once

#include <kernel/sched/sched.h>
#include <kernel/types.h>

/* Thread creation flags */
#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000

/* Function prototypes */
struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, ...);
void kthread_run(struct task_struct *k);

pid_t sys_fork(void);
void sys_exit(int error_code);

/* Internal helpers */
struct task_struct *copy_process(unsigned long clone_flags,
                                 unsigned long stack_start,
                                 unsigned long stack_size,
                                 struct task_struct *p);

void wake_up_new_task(struct task_struct *p);
