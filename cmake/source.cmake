# ============================================================================
# Sources Organization
# ============================================================================
set(INIT_SOURCES
    init/main.c
)

set(KERNEL_SOURCES
    kernel/panic.c
)

set(DRIVER_SOURCES
    drivers/uart/serial.c
)

set(LIB_SOURCES
    lib/string.c
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
)
