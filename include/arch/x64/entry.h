#pragma once

#include <kernel/types.h>

// Syscall Registers (matches ASM stack layout in syscall.asm)
struct syscall_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11_dup, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

/**
 * Initializes syscall MSRs (STAR, LSTAR, FMASK, EFER.SCE).
 */
void syscall_init(void);
