#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    struct list_head entry;
    work_func_t func;
    void *data;
    uint32_t flags;
};

#define WORK_STRUCT_PENDING_BIT 0
#define WORK_STRUCT_PENDING (1 << WORK_STRUCT_PENDING_BIT)

struct workqueue_struct {
    struct list_head worklist;
    spinlock_t lock;
    struct task_struct *worker;
    wait_queue_head_t wait;
    const char *name;
};

/**
 * INIT_WORK - Initialize a work structure
 */
#define INIT_WORK(_work, _func) \
    do { \
        INIT_LIST_HEAD(&(_work)->entry); \
        (_work)->func = (_func); \
        (_work)->flags = 0; \
    } while (0)

/**
 * schedule_work - Queue work on the system default workqueue
 */
bool schedule_work(struct work_struct *work);

/**
 * create_workqueue - Create a new workqueue
 */
struct workqueue_struct *create_workqueue(const char *name);

/**
 * queue_work - Queue work on a specific workqueue
 */
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);

/**
 * workqueue_init - Initialize the workqueue system
 */
int workqueue_init(void);

