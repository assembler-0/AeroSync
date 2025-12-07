[bits 64]

global gdt_flush
global tss_flush

gdt_flush:
    ; Preserve flags and disable interrupts during the GDT switch
    pushfq              ; Save RFLAGS
    cli                 ; Avoid interrupts during segment reloads (no IDT yet)

    lgdt [rdi]          ; Load new GDT

    ; Far jump to reload CS first (required after LGDT)
    push 0x08           ; Kernel code selector
    lea rax, [rel .flush]
    push rax
    retfq               ; 64-bit far return (retfq)

.flush:
    ; Now reload data segment registers
    mov ax, 0x10        ; Kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax          ; CRITICAL: SS must be a valid data selector, NOT NULL!
    popfq               ; Restore original RFLAGS (re-enables IF if it was set)
    ret

tss_flush:
    mov ax, 0x28        ; TSS selector (index 5 * 8 = 40 = 0x28)
    ltr ax              ; Load Task Register
    ret