#pragma once

#include <aerosync/types.h>

struct syscall_regs;

#define NSIG 64

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED 31

/* These should be in sync with sigset_t */
typedef uint64_t sigset_t;

#define sigmask(sig) (1ULL << ((sig) - 1))

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SIG_BLOCK          0
#define SIG_UNBLOCK        1
#define SIG_SETMASK        2

struct sigaction {
    void (*sa_handler)(int);
    uint64_t sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

/* Internal representation of signal handlers */
struct k_sigaction {
    struct sigaction sa;
};

struct signal_struct {
    int count; /* Reference count */
    struct k_sigaction action[NSIG];
    /* Other process-wide signal state can go here */
};

struct sigpending {
    sigset_t signal;
    /* In a full implementation, this would have a list of siginfo_t */
};

/* Function prototypes */
struct task_struct;
void signal_init_task(struct task_struct *p);
int send_signal(int sig, struct task_struct *p);
void handle_signal(struct task_struct *p, int sig, sigset_t *oldset);
void do_signal(void *regs, bool is_syscall);

/* Architecture specific */
void arch_setup_sigframe(void *regs, bool is_syscall, int sig, sigset_t *oldset);

/* System Calls */
void sys_rt_sigaction(struct syscall_regs *regs);
void sys_rt_sigprocmask(struct syscall_regs *regs);
void sys_rt_sigreturn(struct syscall_regs *regs);
void sys_kill(struct syscall_regs *regs);
void sys_tkill(struct syscall_regs *regs);
void sys_tgkill(struct syscall_regs *regs);
