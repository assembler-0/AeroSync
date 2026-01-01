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
#include <kernel/sched/process.h>
#include <kernel/types.h>
#include <kernel/sysintf/panic.h>
#include <lib/printk.h>

#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MSR_EFER 0xC0000080
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define EFER_SCE 0x01

extern void syscall_entry(void);

#define REGS_RETURN_VAL(r, v) (r ? r->rax = v : panic(SYSCALL_CLASS "regs == null"))

// Syscall Registers (matches ASM stack layout)
struct syscall_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11_dup, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*sys_call_ptr_t)(struct syscall_regs *);

static void sys_ni_syscall(struct syscall_regs *regs) {
    printk(KERN_WARNING SYSCALL_CLASS "Unknown syscall %llu\n", regs->rax);
    REGS_RETURN_VAL(regs, -1);
}

static void sys_write(struct syscall_regs *regs) {
    REGS_RETURN_VAL(regs, 0);
}

static void sys_exit_handler(struct syscall_regs *regs) {
    sys_exit((int)regs->rdi);
}

static sys_call_ptr_t syscall_table[] = {
    [1] = sys_write,
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