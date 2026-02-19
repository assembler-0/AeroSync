# ============================================================================
# Sources Organization
# ============================================================================
include(lib/uACPI/uacpi.cmake)

file(GLOB ARCH_SOURCES "arch/x86_64/*.c")
file(GLOB_RECURSE ARCH_FEATURE_SOURCES "arch/x86_64/features/*.c")
file(GLOB_RECURSE ARCH_GDT_SOURCES "arch/x86_64/gdt/*.c")
file(GLOB_RECURSE ARCH_IDT_SOURCES "arch/x86_64/idt/*.c")
file(GLOB_RECURSE ARCH_MM_SOURCES "arch/x86_64/mm/*.c")
file(GLOB_RECURSE ARCH_IRQ_SOURCES "arch/x86_64/irq/*.c")
file(GLOB_RECURSE ARCH_ENTRY_SOURCES "arch/x86_64/entry/*.c")
file(GLOB_RECURSE ARCH_LIB_SOURCES "arch/x86_64/lib/*.c")
file(GLOB_RECURSE ARCH_ASM_SOURCES "arch/x86_64/*.asm")
list(APPEND ARCH_SOURCES
    ${ARCH_FEATURE_SOURCES}
    ${ARCH_GDT_SOURCES}
    ${ARCH_IDT_SOURCES}
    ${ARCH_MM_SOURCES}
    ${ARCH_IRQ_SOURCES}
    ${ARCH_ENTRY_SOURCES}
    ${ARCH_LIB_SOURCES}
    ${ARCH_ASM_SOURCES}
)
file(GLOB_RECURSE INIT_SOURCES "init/*.c")
file(GLOB KERNEL_SOURCES "aerosync/*.c")
file(GLOB_RECURSE KERNEL_SCHED_SOURCES "aerosync/sched/*.c")
file(GLOB_RECURSE KERNEL_FKX_SOURCES "aerosync/fkx/*.c")
file(GLOB_RECURSE KERNEL_ASRX_SOURCES "aerosync/asrx/*.c")
file(GLOB KERNEL_SYSINTF_CORE_SOURCES "aerosync/sysintf/*.c")
file(GLOB_RECURSE KERNEL_ASM_SOURCES "aerosync/*.asm")
file(GLOB_RECURSE KERNEL_SYSINTF_SOURCES "aerosync/sysintf/core/*.c")
list(APPEND KERNEL_SOURCES
    ${KERNEL_SCHED_SOURCES}
    ${KERNEL_ASRX_SOURCES}
    ${KERNEL_FKX_SOURCES}
    ${KERNEL_SYSINTF_CORE_SOURCES}
    ${KERNEL_SYSINTF_SOURCES}
    ${KERNEL_ASM_SOURCES}
)
file(GLOB DRIVER_SOURCES "drivers/*.c")
file(GLOB_RECURSE DRIVER_ACPI_SOURCES "drivers/acpi/*.c")
file(GLOB_RECURSE DRIVER_QEMU_SOURCES "drivers/qemu/*.c")
list(APPEND DRIVER_SOURCES
    ${DRIVER_ACPI_SOURCES}
    ${DRIVER_QEMU_SOURCES}
)
file(GLOB LIB_SOURCES "lib/*.c")
file(GLOB MM_SOURCES "mm/*.c")
file(GLOB_RECURSE MM_SAN_SOURCES "mm/san/*.c")
list(APPEND MM_SOURCES
    ${MM_SAN_SOURCES}
)
file(GLOB CRYPTO_SOURCES "crypto/*.c" "crypto/sha/*.c")
file(GLOB FS_SOURCES "fs/*.c")

# ============================================================================
# Build Include Directories
# ============================================================================
include_directories(SYSTEM
        .
        include
        include/lib
        ${CMAKE_BINARY_DIR}
        ${UACPI_INCLUDES}
)

# ============================================================================
# Final Source List
# ============================================================================
set(AEROSYNC_SOURCES
        ${INIT_SOURCES}
        ${KERNEL_SOURCES}
        ${LIB_SOURCES}
        ${DRIVER_SOURCES}
        ${MM_SOURCES}
        ${ARCH_SOURCES}
        ${CRYPTO_SOURCES}
        ${FS_SOURCES}
        ${UACPI_SOURCES}
)
