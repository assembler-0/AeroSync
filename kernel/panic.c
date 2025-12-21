/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/panic.c
 * @brief Panic handling functions
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <arch/x64/cpu.h>
#include <arch/x64/exception.h>
#include <compiler.h>
#include <kernel/classes.h>
#include <lib/printk.h>

#define PANIC KERN_EMERG PANIC_CLASS

void __exit __noinline __noreturn __sysv_abi panic_early() {
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi panic(const char *msg) {
    printk(PANIC"panic - not syncing: %s", msg);
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi panic_exception(cpu_regs *regs) {
    char exception[256];
    get_exception_as_str(exception, regs->interrupt_number);

    printk(PANIC"-----------------------------------------------------"
                            "---------------------------\n");
    printk(PANIC
            "panic - not syncing - exception: %s (V: 0x%llx EC: 0x%llx)\n",
            exception, regs->interrupt_number, regs->error_code);
    printk(PANIC"----------------------------------------------------------"
                    "----------------------\n");

    // Dump Execution State
    printk(PANIC"Context:\n");
    printk(PANIC"  RIP: %llx  CS: %llx  RFLAGS: %llx\n", regs->rip,
            regs->cs, regs->rflags);
    printk(PANIC"  RSP: %llx  SS: %llx\n", regs->rsp, regs->ss);

    // Dump General Purpose Registers
    printk(PANIC"General Purpose Registers:\n");
    printk(PANIC"  RAX: %llx  RBX: %llx  RCX: %llx\n", regs->rax,
            regs->rbx, regs->rcx);
    printk(PANIC"  RDX: %llx  RSI: %llx  RDI: %llx\n", regs->rdx,
            regs->rsi, regs->rdi);
    printk(PANIC"  RBP: %llx  R8 : %llx  R9 : %llx\n", regs->rbp,
            regs->r8, regs->r9);
    printk(PANIC"  R10: %llx  R11: %llx  R12: %llx\n", regs->r10,
            regs->r11, regs->r12);
    printk(PANIC"  R13: %llx  R14: %llx  R15: %llx\n", regs->r13,
            regs->r14, regs->r15);

    // Dump Segments
    printk(PANIC"Segment Registers:\n");
    printk(PANIC"  DS: %llx  ES: %llx  FS: %llx  GS: %llx\n",
            regs->ds, regs->es, regs->fs, regs->gs);

    // Dump Control Registers
    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    printk(PANIC"Control Registers:\n");
    printk(PANIC"  CR0: %llx  CR2: %llx\n", cr0, cr2);
    printk(PANIC"  CR3: %llx  CR4: %llx\n", cr3, cr4);

    printk(PANIC"----------------------------------------------------------"
                    "----------------------\n");

    system_hlt();
    __unreachable();
}