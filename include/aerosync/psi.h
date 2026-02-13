#pragma once

#include <aerosync/types.h>
#include <aerosync/sched/process.h>

enum psi_res {
	PSI_IO,
	PSI_MEM,
	PSI_CPU,
	PSI_NR,
};

enum psi_states {
	PSI_IO_SOME,
	PSI_IO_FULL,
	PSI_MEM_SOME,
	PSI_MEM_FULL,
	PSI_CPU_SOME,
	/* Only per-CPU, to account for non-idle time */
	PSI_NONIDLE,
	PSI_STATE_NR,
};

struct psi_group_cpu {
	uint32_t tasks[PSI_NR];
	uint32_t state_mask;
	uint64_t state_start;
};

struct psi_group {
	struct psi_group_cpu __percpu *pcpu;
};

#ifdef CONFIG_PSI
void psi_task_change(struct task_struct *task, int clear, int set);
void psi_memstall_enter(unsigned long *flags);
void psi_memstall_leave(unsigned long *flags);
#else
static inline void psi_task_change(struct task_struct *task, int clear, int set) {}
static inline void psi_memstall_enter(unsigned long *flags) { (void)flags; }
static inline void psi_memstall_leave(unsigned long *flags) { (void)flags; }
#endif
