[bits 64]

%define MSR_GS_BASE     0xC0000101
%define MSR_KERNEL_GS_BASE 0xC0000102

section .text
global syscall_entry
extern do_syscall
extern tss_entry
extern cpu_user_rsp

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
    ; Layout:
    ; struct syscall_regs {
    ;     uint64_t r15, r14, r13, r12, rbp, rbx;
    ;     uint64_t r11, r10, r9, r8;
    ;     uint64_t rdx, rsi, rdi, rax;
    ;     uint64_t rip, cs, rflags, rsp, ss;
    ; };
    ; Note: SYSCALL doesn't push SS/RSP/CS/RIP automatically. We simulate an interrupt frame.
    
    ; Simulated Interrupt Frame (SS, RSP, RFLAGS, CS, RIP)
    push 0x1B                   ; User SS (User Data + 3)
    push qword [gs:cpu_user_rsp]; User RSP
    push r11                    ; Saved RFLAGS
    push 0x23                   ; User CS (User Code + 3)
    push rcx                    ; Saved RIP
    
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
    ; Stack Top: [RIP] [CS] [RFLAGS] [RSP] [SS]
    
    pop rcx     ; RCX = RIP
    add rsp, 8  ; Skip CS
    pop r11     ; R11 = RFLAGS
    pop rsp     ; RSP = User RSP
    
    swapgs      ; Switch to User GS
    
    o64 sysret  ; Return to Ring 3 (64-bit)