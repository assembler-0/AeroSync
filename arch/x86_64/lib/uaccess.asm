; SPDX-License-Identifier: GPL-2.0-only
;
; VoidFrameX monolithic kernel
;
; @file arch/x86_64/lib/uaccess.asm
; @brief User memory access routines with exception handling
; @copyright (C) 2025 assembler-0

section .text

global __copy_from_user
global __copy_to_user

; size_t __copy_from_user(void *to [rdi], const void *from [rsi], size_t n [rdx])
__copy_from_user:
    test rdx, rdx
    jz .done
    
    mov rcx, rdx
.loop:
.copy_in:
    mov al, byte [rsi]       ; Fault can happen here (reading user memory)
    mov byte [rdi], al
    inc rsi
    inc rdi
    dec rcx
    jnz .loop

.done:
    xor rax, rax
    ret

.fixup:
    mov rax, rcx
    ret

section __ex_table alloc align=8
    dq .copy_in, __copy_from_user.fixup
section .text

; size_t __copy_to_user(void *to [rdi], const void *from [rsi], size_t n [rdx])
__copy_to_user:
    test rdx, rdx
    jz .done_to
    
    mov rcx, rdx
.loop_to:
    mov al, byte [rsi]
.copy_out:
    mov byte [rdi], al       ; Fault can happen here (writing user memory)
    inc rsi
    inc rdi
    dec rcx
    jnz .loop_to

.done_to:
    xor rax, rax
    ret

.fixup_to:
    mov rax, rcx
    ret

section __ex_table
    dq .copy_out, __copy_to_user.fixup_to