# ============================================================================
# Sources Organization
# ============================================================================
file(GLOB_RECURSE ASM_SOURCES "arch/*.asm" "kernel/*.asm")
file(GLOB_RECURSE ARCH_SOURCES "arch/*.c")
file(GLOB_RECURSE INIT_SOURCES "init/*.c")
file(GLOB_RECURSE KERNEL_SOURCES "kernel/*.c")
file(GLOB_RECURSE DRIVER_SOURCES "drivers/*.c")
file(GLOB_RECURSE LIB_SOURCES "lib/*.c")
file(GLOB_RECURSE MM_SOURCES "mm/*.c")
file(GLOB_RECURSE CRYPTO_SOURCES "crypto/*.c")
file(GLOB_RECURSE FS_SOURCES "fs/*.c")

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
        ${FS_SOURCES}
)
