#include <arch/x64/mm/pmm.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <lib/string.h>

/*
 * Process/Thread Management
 */

extern void activate_task(struct rq *rq, struct task_struct *p);
extern struct rq *this_rq(void);

extern char _text_start[];
extern char _text_end[];
extern char _rodata_start[];
extern void ret_from_kernel_thread(void);

extern void vmm_dump_entry(uint64_t pml4_phys, uint64_t virt);
extern uint64_t g_kernel_pml4;

// Defined in switch.S
void __switch_to(struct thread_struct *prev, struct thread_struct *next);

void switch_to(struct task_struct *prev, struct task_struct *next) {
  if (prev == next)
    return;
  __switch_to(&prev->thread, &next->thread);
}

// Entry point for new kernel threads
void kthread_entry_stub(int (*threadfn)(void *data), void *data) {
  cpu_sti();

  threadfn(data);

  sys_exit(0);
}

struct task_struct *kthread_create(int (*threadfn)(void *data), void *data,
                                   const char *namefmt, ...) {
  // Allocate task_struct
  // For now, valid memory check?
  // We need a slab allocator ideally, but let's use pmm/vmm or a simple malloc
  // if available. Since we don't have kmalloc yet, we might have to use
  // pmm_alloc_page and cast? That's wasteful (4KB for a task struct), but
  // works.

  uint64_t page_phys = pmm_alloc_pages(1); // 1 page
  if (!page_phys)
    return NULL;

  struct task_struct *ts = (struct task_struct *)pmm_phys_to_virt(
      page_phys); // We need access to it.

  memset(ts, 0, sizeof(*ts));

  // Allocate Stack
  uint64_t stack_phys = pmm_alloc_pages(2); // 8KB stack
  if (!stack_phys) {
    // free ts
    pmm_free_pages(page_phys, 1); // Free the task_struct page
    return NULL;
  }

  void *stack = pmm_phys_to_virt(stack_phys);

  ts->stack = stack;
  ts->flags = PF_KTHREAD;
  ts->state = TASK_RUNNING;
  ts->prio = DEFAULT_PRIO;
  ts->se.vruntime = 0; // Should inherit or be set fairly

  // Initialize list heads
  INIT_LIST_HEAD(&ts->run_list);
  INIT_LIST_HEAD(&ts->tasks);
  INIT_LIST_HEAD(&ts->sibling);
  INIT_LIST_HEAD(&ts->children);

  // Setup Thread Context
  uint64_t *sp = (uint64_t *)(stack + 4096 * 2);

  // Simulate the stack frame for __switch_to
  /*
      buffer:
      ret addr -> kthread_entry_stub
      rbx, rbp, r12, r13, r14, r15 -> 0
  */

  *(--sp) = (uint64_t)kthread_entry_stub; // Return address (rip)

  // Registers popped by __switch_to
  *(--sp) = 0; // r15
  *(--sp) = 0; // r14
  *(--sp) = 0; // r13
  *(--sp) = 0; // r12
  *(--sp) = 0; // rbp
  *(--sp) = 0; // rbx

  ts->thread.rsp = (uint64_t)sp;

  // We need to pass arguments to kthread_entry_stub.
  // SysV ABI: rdi = arg1, rsi = arg2.
  // __switch_to doesn't restore RDI/RSI.
  // We need a trampoline helper that moves popped values to RDI/RSI?
  // OR we change __switch_to to also save/restore RDI/RSI (callee saved in
  // Windows, but volatile in SysV?) In SysV, RDI/RSI are caller-saved.

  // SOLUTION: Use a trampoline in assembly that is the "return address".
  // "ret" jumps to ret_from_fork defined in entry.S, which handles args.
  // But simplest way:
  // make r12 = fn, r13 = data (since they are callee saved and restored!)
  // Update kthread_entry_stub to take args from r12/r13? No, it's a C function.

  // Let's modify the stack setup to use a small assembly helper
  // 'fast_kthread_start' that moves r12->rdi, r13->rsi, then calls the C
  // function.

  // Update the stack:
  sp = (uint64_t *)(stack + 4096 * 2);
  *(--sp) = (uint64_t)ret_from_kernel_thread;

  *(--sp) = 0;                  // rbx
  *(--sp) = 0;                  // rbp
  *(--sp) = (uint64_t)threadfn; // r12 -> becomes rdi
  *(--sp) = (uint64_t)data;     // r13 -> becomes rsi
  *(--sp) = 0;                  // r14
  *(--sp) = 0;                  // r15

  ts->thread.rsp = (uint64_t)sp;

  return ts;
}

void kthread_run(struct task_struct *k) {
  if (!k)
    return;

  struct rq *rq = this_rq();
  unsigned long flags = spinlock_lock_irqsave((volatile int *)&rq->lock);

  activate_task(rq, k);

  spinlock_unlock_irqrestore((volatile int *)&rq->lock, flags);
}

void sys_exit(int error_code) {
  // Very basic exit
  struct task_struct *curr = get_current();
  curr->state = TASK_ZOMBIE;

  // Deschedule
  schedule();

  // Should never reach here
  while (1)
    ;
}
