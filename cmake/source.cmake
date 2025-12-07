# ============================================================================
# Sources Organization
# ============================================================================
set(ASM_SOURCES
    arch/x64/gdt_tss.asm
)

set(ARCH_SOURCES
    arch/x64/cpu.c
    arch/x64/gdt/gdt.c
    arch/x64/idt/idt.c
    arch/x64/idt/isr.asm
    arch/x64/idt/load_idt.asm
    arch/x64/smp.c
    arch/x64/irq/irq.c
    arch/x64/exception.c
)

set(INIT_SOURCES
    init/main.c
)

set(KERNEL_SOURCES
    kernel/panic.c
)

set(DRIVER_SOURCES
    drivers/uart/serial.c
    drivers/apic/apic.c
)

set(LIB_SOURCES
    lib/string.c
    lib/printk.c
    lib/log.c
    lib/vsprintf.c
    lib/ctype.c
)

set(MM_SOURCES
    mm/pmm.c
    mm/vmm.c
)

set(CRYPTO_SOURCES
    crypto/crc32.c
    crypto/rng.c
    crypto/sha/sha256.c
)

# ============================================================================
# Build Include Directories
# ============================================================================
include_directories(
        .
        include
        include/lib
)

# ============================================================================
# Final Source List
# ============================================================================
set(C_SOURCES
        ${INIT_SOURCES}
        ${KERNEL_SOURCES}
        ${LIB_SOURCES}
        ${DRIVER_SOURCES}
        ${MM_SOURCES}
        ${ARCH_SOURCES}
        ${CRYPTO_SOURCES}
)
