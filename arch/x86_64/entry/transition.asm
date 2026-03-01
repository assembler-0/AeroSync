[bits 64]
default rel

section .text

global enter_userspace

; void enter_userspace(struct cpu_regs *regs)
; rdi = pointer to cpu_regs
enter_userspace:
    ; Disable interrupts to prevent entering an ISR with user GS base
    cli

    ; Switch stack to the regs pointer
    mov rsp, rdi

    ; Swap to user GS NOW.
    ; This places the Kernel's per-CPU base into MSR_KERNEL_GS_BASE safely.
    ; The active GS base becomes the User GS base.
    swapgs

    ; Pop segment registers
    ; In x86-64 long mode, pop ds/es are invalid.
    ; We skip them (already adjusted in C if needed)
    add rsp, 16 ; skip ds, es

    ; Popping FS/GS clears the *active* base. Because we swapped GS,
    ; this clears the User GS Base, leaving the Kernel GS Base unharmed.
    pop fs
    pop gs

    ; Pop General Purpose Registers
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rdi
    pop rsi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; Skip interrupt_number and error_code
    add rsp, 16

    ; Return to user-space
    ; iretq pops: RIP, CS, RFLAGS, RSP, SS
    iretq
