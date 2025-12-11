[bits 64]

section .text

global __switch_to
global ret_from_kernel_thread

extern kthread_entry_stub

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
; rdi = fn, rsi = data (restored from stack into r12/r13 by stack setup in kthread_create)
ret_from_kernel_thread:
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
