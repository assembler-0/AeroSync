# ============================================================================
# Sources Organization
# ============================================================================
set(ARCH_SOURCES
    arch/x64/cpu.c
    arch/x64/gdt.c
)

set(INIT_SOURCES
    init/main.c
)

set(KERNEL_SOURCES
    kernel/panic.c
    kernel/cmdline.c
)

set(DRIVER_SOURCES
    drivers/uart/serial.c
)

set(LIB_SOURCES
    lib/string.c
    lib/printk.c
    lib/log.c
)

set(MM_SOURCES
    mm/pmm.c
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
)
