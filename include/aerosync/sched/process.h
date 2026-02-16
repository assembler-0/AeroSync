#pragma once

#include <aerosync/sched/sched.h>
#include <aerosync/types.h>

struct syscall_regs;

/* Thread creation flags */
#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000
#define CLONE_NEWPID 0x20000000
#define CLONE_KSTACK 0x40000000

/* Global task list */
extern struct list_head task_list;
extern spinlock_t tasklist_lock;

/* Function prototypes */
int pid_allocator_init(void);
struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, ...);
void kthread_run(struct task_struct *k);
void set_task_cpu(struct task_struct *task, int cpu);
void move_task_to_rq(struct task_struct *task, int dest_cpu);

void spawn_user_test_process(void);

#ifdef CONFIG_UNSAFE_USER_TASK_SPAWN
/**
 * @warning USE THIS FUNCTION FOR INTERNAL PURPOSES ONLY
 */
struct task_struct * __deprecated spawn_user_process_raw(void *data, size_t len, const char *name);
#endif

struct file;

int do_execve_from_buffer(void *data, size_t len, const char *name);
int do_execve_file(struct file *file, const char *name, char **argv, char **envp);
int do_execve(const char *filename, char **argv, char **envp);
int run_init_process(const char *init_filename);

pid_t sys_fork(void);
pid_t do_fork(uint64_t clone_flags, uint64_t stack_start, struct syscall_regs *regs);

struct task_struct *process_spawn(int (*entry)(void *), void *data,
                                  const char *name);
void sys_exit(int error_code);
void free_task(struct task_struct *task);

/* Internal helpers */
struct task_struct *copy_process(uint64_t clone_flags,
                                 uint64_t stack_start,
                                 struct task_struct *p);

void wake_up_new_task(struct task_struct *p);

/* Task memory management */
struct task_struct *alloc_task_struct(void);
void free_task_struct(struct task_struct *task);