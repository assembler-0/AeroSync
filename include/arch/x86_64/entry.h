#pragma once

#include <aerosync/types.h>
#include <arch/x86_64/cpu.h>

// Syscall Registers (matches ASM stack layout in syscall.asm)
struct syscall_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11_dup, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

/**
 * Initializes syscall MSRs (STAR, LSTAR, FMASK, EFER.SCE).
 */
int syscall_init(void);

/**
 * Transition to user-space using the provided register state.
 * This function does not return.
 * 
 * @param regs Pointer to cpu_regs structure on the kernel stack.
 */
void enter_userspace(struct cpu_regs *regs) __noreturn;
