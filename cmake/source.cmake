# ============================================================================
# Sources Organization
# ============================================================================
file(GLOB ARCH_SOURCES "arch/x64/*.c")
file(GLOB_RECURSE ARCH_FEATURE_SOURCES "arch/x64/features/*.c")
file(GLOB_RECURSE ARCH_GDT_SOURCES "arch/x64/gdt/*.c")
file(GLOB_RECURSE ARCH_IDT_SOURCES "arch/x64/idt/*.c")
file(GLOB_RECURSE ARCH_MM_SOURCES "arch/x64/mm/*.c")
file(GLOB_RECURSE ARCH_IRQ_SOURCES "arch/x64/irq/*.c")
file(GLOB_RECURSE ARCH_ASM_SOURCES "arch/x64/*.asm")
list(APPEND ARCH_SOURCES
    ${ARCH_FEATURE_SOURCES}
    ${ARCH_GDT_SOURCES}
    ${ARCH_IDT_SOURCES}
    ${ARCH_MM_SOURCES}
    ${ARCH_IRQ_SOURCES}
    ${ARCH_ASM_SOURCES}
)
file(GLOB_RECURSE INIT_SOURCES "init/*.c")
file(GLOB KERNEL_SOURCES "kernel/*.c")
file(GLOB_RECURSE KERNEL_SCHED_SOURCES "kernel/sched/*.c")
file(GLOB_RECURSE KERNEL_FKX_SOURCES "kernel/fkx/*.c")
file(GLOB_RECURSE KERNEL_ASM_SOURCES "kernel/*.asm")
list(APPEND KERNEL_SOURCES
    ${KERNEL_SCHED_SOURCES}
    ${KERNEL_FKX_SOURCES}
    ${KERNEL_ASM_SOURCES}
)
file(GLOB DRIVER_SOURCES "drivers/*.c")
file(GLOB_RECURSE DRIVER_PCI_SOURCES "drivers/pci/*.c")
file(GLOB_RECURSE DRIVER_ACPI_SOURCES "drivers/acpi/*.c")
file(GLOB_RECURSE DRIVER_TIMER_SOURCES "drivers/timer/*.c")
file(GLOB_RECURSE DRIVER_QEMU_SOURCES "drivers/qemu/*.c")
list(APPEND DRIVER_SOURCES
    ${DRIVER_PCI_SOURCES}
    ${DRIVER_ACPI_SOURCES}
    ${DRIVER_TIMER_SOURCES}
    ${DRIVER_QEMU_SOURCES}
)
file(GLOB LIB_SOURCES "lib/*.c")
file(GLOB MM_SOURCES "mm/*.c")
file(GLOB_RECURSE MM_SAN_SOURCES "mm/san/*.c")
list(APPEND MM_SOURCES
    ${MM_SAN_SOURCES}
)
file(GLOB CRYPTO_SOURCES "crypto/*.c")
file(GLOB FS_SOURCES "fs/*.c")

# ============================================================================
# Build Include Directories
# ============================================================================
include_directories(
        .
        include
        include/lib
        ${UACPI_INCLUDES}
)

# ============================================================================
# Final Source List
# ============================================================================
set(VOIDFRAMEX_SOURCES
        ${INIT_SOURCES}
        ${KERNEL_SOURCES}
        ${LIB_SOURCES}
        ${DRIVER_SOURCES}
        ${MM_SOURCES}
        ${ARCH_SOURCES}
        ${CRYPTO_SOURCES}
        ${FS_SOURCES}
)
