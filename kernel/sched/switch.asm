[bits 64]

section .text

global __switch_to
global ret_from_kernel_thread
global ret_from_user_thread

extern kthread_entry_stub
extern schedule_tail

; void __switch_to(struct thread_struct *prev, struct thread_struct *next);
; rdi = prev, rsi = next
__switch_to:
    ; Save Callee-Saved Registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    
    ; Save current Stack Pointer to prev->rsp
    mov [rdi], rsp
    
    ; Return 'prev' (the one being switched out) in RAX
    mov rax, rdi
    
    ; Load next Stack Pointer from next->rsp
    mov rsp, [rsi]
    
    ; Restore Callee-Saved Registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    
    ret

; entry point for new kernel threads
; rax = prev (returned from __switch_to)
ret_from_kernel_thread:
    ; Finish schedule() by releasing lock and enabling IRQs
    mov rdi, rax
    call schedule_tail

    ; kthread_create pushed: [fn (r12)] [data (r13)] [0..0]
    ; __switch_to pops r12..r15.
    ; So r12 contains fn, r13 contains data.
    
    mov rdi, r12  ; fn
    mov rsi, r13  ; data
    
    ; We call the stub, which calls the function and then exit
    ; We need to call kthread_entry_stub(fn, data)
    
    call kthread_entry_stub
    
    ; should not return
    hlt

; entry point for new user threads
; rax = prev
ret_from_user_thread:
    ; Finish schedule()
    mov rdi, rax
    call schedule_tail

    ; The stack contains cpu_regs.
    ; We skip segment registers (ds, es, fs, gs) and int info (num, err)
    ; because they were either not set or we handle them manually.
    ; Offset to GPRs is 32 bytes (4 segments * 8).
    add rsp, 32

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

    ; Swap to user GS
    swapgs

    ; Return to user-space
    iretq
