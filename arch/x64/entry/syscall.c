///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/entry/syscall.c
 * @brief System Call Dispatcher and Initialization
 * @copyright (C) 2025 assembler-0
 */

#include <arch/x64/cpu.h>
#include <arch/x64/gdt/gdt.h>
#include <kernel/classes.h>
#include <kernel/errno.h>
#include <kernel/sched/process.h>
#include <kernel/types.h>
#include <kernel/sysintf/panic.h>
#include <lib/printk.h>
#include <lib/uaccess.h>
#include <arch/x64/entry.h>

#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MSR_EFER 0xC0000080
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define EFER_SCE 0x01

extern void syscall_entry(void);

#define REGS_RETURN_VAL(r, v) (r ? r->rax = v : panic(SYSCALL_CLASS "regs == null"))

fnd(void, sys_call_ptr_t, struct syscall_regs *);

static void sys_ni_syscall(struct syscall_regs *regs) {
    printk(KERN_WARNING SYSCALL_CLASS "Unknown syscall %llu\n", regs->rax);
    REGS_RETURN_VAL(regs, -1);
}

static void sys_write(struct syscall_regs *regs) {
    int fd = (int)regs->rdi;
    const char *buf = (const char *)regs->rsi;
    size_t count = (size_t)regs->rdx;

    if (fd == 1 || fd == 2) { // stdout or stderr
        char kbuf[256];
        size_t total = 0;
        
        while (count > 0) {
            size_t to_copy = (count > sizeof(kbuf) - 1) ? sizeof(kbuf) - 1 : count;
            if (copy_from_user(kbuf, buf, to_copy) != 0) {
                REGS_RETURN_VAL(regs, -EFAULT);
                return;
            }
            
            kbuf[to_copy] = '\0';
            printk(KERN_INFO "%s", kbuf); // TODO: NOT JUST PRINTK!, IMPLEMENT WRITE!
            
            buf += to_copy;
            count -= to_copy;
            total += to_copy;
        }
        REGS_RETURN_VAL(regs, total);
    } else {
        REGS_RETURN_VAL(regs, -EBADF);
    }
}

static void sys_exit_handler(struct syscall_regs *regs) {
    int status = (int)regs->rdi;
    printk(KERN_DEBUG SYSCALL_CLASS "User process %d exited with status %d\n", current->pid, status);
    sys_exit(status);
}

static void sys_fork_handler(struct syscall_regs *regs) {
    REGS_RETURN_VAL(regs, do_fork(0, 0, regs));
}

static void sys_clone_handler(struct syscall_regs *regs) {
    uint64_t flags = regs->rdi;
    uint64_t stack = regs->rsi;
    REGS_RETURN_VAL(regs, do_fork(flags, stack, regs));
}

static void sys_getpid_handler(struct syscall_regs *regs) {
    REGS_RETURN_VAL(regs, current->pid);
}

static sys_call_ptr_t syscall_table[] = {
    [1] = sys_write,
    [39] = sys_getpid_handler,
    [56] = sys_clone_handler,
    [57] = sys_fork_handler,
    [60] = sys_exit_handler,
};

#define NR_SYSCALLS (sizeof(syscall_table) / sizeof(sys_call_ptr_t))

void do_syscall(struct syscall_regs *regs) {
    uint64_t syscall_num = regs->rax;
    
    if (syscall_num >= NR_SYSCALLS || !syscall_table[syscall_num]) {
        sys_ni_syscall(regs);
        return;
    }

    syscall_table[syscall_num](regs);
}

void syscall_init(void) {
    // 1. Enable SCE (Syscall Extensions) in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // 2. Setup STAR (Segment Target Address Register)
    // Bits 63-48: Sysret CS (User Code - 16). 
    // Bits 47-32: Syscall CS (Kernel Code).
    uint64_t star = 0;
    star |= ((uint64_t)KERNEL_DATA_SELECTOR << 48); // User Base (0x10) -> CS=0x20, SS=0x18
    star |= ((uint64_t)KERNEL_CODE_SELECTOR << 32); // Kernel Base (0x08) -> CS=0x08, SS=0x10
    wrmsr(MSR_STAR, star);

    // 3. Setup LSTAR (Long Mode Syscall Target Address)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // 4. Setup SFMASK (RFLAGS Mask)
    // Mask interrupts (IF=0x200), Direction (DF=0x400)
    wrmsr(MSR_FMASK, 0x200); 

    // 5. Initialize KERNEL_GS_BASE with current GS_BASE
    // This ensures that the first swapgs in enter_ring3 has a valid kernel GS to swap back.
    wrmsr(MSR_KERNEL_GS_BASE, rdmsr(MSR_GS_BASE));
    
    printk(KERN_DEBUG SYSCALL_CLASS "Syscall infrastructure initialized.\n");
}