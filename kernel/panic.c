#include <arch/x64/cpu.h>
#include <arch/x64/exception.h>
#include <compiler.h>
#include <kernel/classes.h>
#include <lib/printk.h>

void __exit __noinline __noreturn __sysv_abi panic_early() {
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi panic(const char *msg) {
    printk(KERN_EMERG PANIC_CLASS "panic - not syncing: %s", msg);
    system_hlt();
    __unreachable();
}

void __exit __noinline __noreturn __sysv_abi panic_exception(cpu_regs *regs) {
    char exception[256];
    get_exception_as_str(exception, regs->interrupt_number);

    printk("\n" KERN_EMERG "-----------------------------------------------------"
                            "---------------------------\n");
    printk(KERN_EMERG PANIC_CLASS
            "EXCEPTION DETECTED: %s (Vector: 0x%lx, Error: 0x%lx)\n",
            exception, regs->interrupt_number, regs->error_code);
    printk(KERN_EMERG PANIC_CLASS "CPU Halting due to fatal exception.\n");
    printk(KERN_EMERG "----------------------------------------------------------"
                    "----------------------\n");

    // Dump Execution State
    printk(KERN_EMERG "Context:\n");
    printk(KERN_EMERG "  RIP: %016llx  CS: %04llx  RFLAGS: %016llx\n", regs->rip,
            regs->cs, regs->rflags);
    printk(KERN_EMERG "  RSP: %016llx  SS: %04llx\n", regs->rsp, regs->ss);

    // Dump General Purpose Registers
    printk(KERN_EMERG "\nGeneral Purpose Registers:\n");
    printk(KERN_EMERG "  RAX: %016llx  RBX: %016llx  RCX: %016llx\n", regs->rax,
            regs->rbx, regs->rcx);
    printk(KERN_EMERG "  RDX: %016llx  RSI: %016llx  RDI: %016llx\n", regs->rdx,
            regs->rsi, regs->rdi);
    printk(KERN_EMERG "  RBP: %016llx  R8 : %016llx  R9 : %016llx\n", regs->rbp,
            regs->r8, regs->r9);
    printk(KERN_EMERG "  R10: %016llx  R11: %016llx  R12: %016llx\n", regs->r10,
            regs->r11, regs->r12);
    printk(KERN_EMERG "  R13: %016llx  R14: %016llx  R15: %016llx\n", regs->r13,
            regs->r14, regs->r15);

    // Dump Segments
    printk(KERN_EMERG "\nSegment Registers:\n");
    printk(KERN_EMERG "  DS: %04llx  ES: %04llx  FS: %04llx  GS: %04llx\n",
            regs->ds, regs->es, regs->fs, regs->gs);

    // Dump Control Registers
    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    printk(KERN_EMERG "\nControl Registers:\n");
    printk(KERN_EMERG "  CR0: %016llx  CR2: %016llx\n", cr0, cr2);
    printk(KERN_EMERG "  CR3: %016llx  CR4: %016llx\n", cr3, cr4);

    printk(KERN_EMERG "----------------------------------------------------------"
                    "----------------------\n");

    system_hlt();
    __unreachable();
}