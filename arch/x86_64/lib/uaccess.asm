; SPDX-License-Identifier: GPL-2.0-only
;
; AeroSync monolithic kernel
;
; @file arch/x86_64/lib/uaccess.asm
; @brief User memory access routines with exception handling
; @copyright (C) 2025-2026 assembler-0

section .text

global __copy_from_user
global __copy_to_user

extern g_cpu_features

%macro SMAP_ALLOW 0
    cmp byte [rel g_cpu_features + 22], 0 ; smap is at offset 22
    jz %%skip
    stac
%%skip:
%endmacro

%macro SMAP_DENY 0
    cmp byte [rel g_cpu_features + 22], 0
    jz %%skip
    clac
%%skip:
%endmacro

; size_t __copy_from_user(void *to [rdi], const void *from [rsi], size_t n [rdx])
__copy_from_user:
    test rdx, rdx
    jz .done
    
    SMAP_ALLOW
    mov rcx, rdx
.loop:
.copy_in:
    mov al, byte [rsi]       ; Fault can happen here (reading user memory)
    mov byte [rdi], al
    inc rsi
    inc rdi
    dec rcx
    jnz .loop
    SMAP_DENY

.done:
    xor rax, rax
    ret

.fixup:
    SMAP_DENY                ; Ensure SMAP is re-enabled on fault
    mov rax, rcx
    ret

section __ex_table alloc align=8
    dq .copy_in, __copy_from_user.fixup
section .text

; size_t __copy_to_user(void *to [rdi], const void *from [rsi], size_t n [rdx])
__copy_to_user:
    test rdx, rdx
    jz .done_to
    
    SMAP_ALLOW
    mov rcx, rdx
.loop_to:
    mov al, byte [rsi]
.copy_out:
    mov byte [rdi], al       ; Fault can happen here (writing user memory)
    inc rsi
    inc rdi
    dec rcx
    jnz .loop_to
    SMAP_DENY

.done_to:
    xor rax, rax
    ret

.fixup_to:
    SMAP_DENY                ; Ensure SMAP is re-enabled on fault
    mov rax, rcx
    ret

section __ex_table
    dq .copy_out, __copy_to_user.fixup_to