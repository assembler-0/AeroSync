[bits 64]
default rel

%define MSR_GS_BASE     0xC0000101
%define MSR_KERNEL_GS_BASE 0xC0000102

section .text
global syscall_entry
extern do_syscall
extern tss_entry
extern cpu_user_rsp
extern this_cpu_off

; Entry point for SYSCALL instruction
syscall_entry:
    ; On entry:
    ; RAX = syscall number
    ; RCX = return RIP
    ; R11 = saved RFLAGS
    ; Arguments: RDI, RSI, RDX, R10, R8, R9

    swapgs                  ; Switch to kernel GS

    ; Save User RSP to per-cpu scratch
    mov [gs:cpu_user_rsp], rsp

    ; Load Kernel RSP from TSS (tss_entry.rsp0 is at offset 4)
    mov rsp, [gs:tss_entry + 4]

    ; Construct struct syscall_regs on stack
    ; Layout needs to be 16-byte aligned for C calls.
    push 0                      ; Alignment Dummy

    ; Simulated Interrupt Frame (SS, RSP, RFLAGS, CS, RIP)
    push 0x1B                   ; User SS (User Data + 3)
    push qword [gs:cpu_user_rsp]; User RSP
    push r11                    ; Saved RFLAGS
    push 0x23                   ; User CS (User Code + 3)
    push rcx                    ; Saved RI

    ; Push GPRs
    push rax    ; Syscall Number
    push rdi    ; Arg 1
    push rsi    ; Arg 2
    push rdx    ; Arg 3
    push r10    ; Arg 4 (RCX is destroyed, R10 holds it)
    push r8     ; Arg 5
    push r9     ; Arg 6
    push r11    ; Saved RFLAGS (duplicated in struct for convenience)

    ; Push remaining callee-saved (for ptrace/debugging)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Load kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Pass regs pointer to do_syscall
    mov rdi, rsp

    ; Enable interrupts
    sti

    call do_syscall

    ; Disable interrupts before exit
    cli

    ; Restore context
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    pop r11 ; saved rflags from struct (discard)
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop rax

    ; Restore Interrupt Frame (RIP, CS, RFLAGS, RSP, SS)
    ; Stack Top: [RIP] [CS] [RFLAGS] [RSP] [SS] [Dummy]

    pop rcx     ; RCX = RIP
    add rsp, 8  ; Skip CS
    pop r11     ; R11 = RFLAGS
    add rsp, 16 ; Skip User RSP and
    add rsp, 8  ; Skip Alignment Dummy

    ; RSP is now back to Kernel Stack top (as it was after TSS switch)
    mov rsp, [gs:cpu_user_rsp] ; Restore User RSP (while still on Kernel GS)
    swapgs                     ; Switch to User GS
    o64 sysret  ; Return to Ring 3 (64-bit)