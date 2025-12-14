# ============================================================================
# VoidFrameX CMake Build Script - v0.0.2-development4
# ============================================================================
cmake_minimum_required(VERSION 3.30)
set_property(GLOBAL PROPERTY RULE_MESSAGES OFF)
project(VoidFrameX
        VERSION 0.0.1
        LANGUAGES C CXX ASM_NASM
        HOMEPAGE_URL "https://github.com/assembler-0/VoidFrameX"
        DESCRIPTION "A hobbyist operating system kernel written in C/C++ Assembly Rust"
)
enable_language(ASM_NASM)

# ============================================================================
# Module Path & Includes & Subdirectories
# ============================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cache)
include(ccache)
include(dependencies)
include(source)
include(configuration)
include(flags)

add_subdirectory(lib/linearfb)

# ============================================================================
# Platform Checks
# ============================================================================
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Detected Linux host system: ${CMAKE_HOST_SYSTEM_NAME}")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    message(WARNING "Windows detected. Install WSL or use a cross-compiler.")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(WARNING "macOS detected. Please make sure you have compatible tools.")
else()
    message(WARNING "Unsupported host system: ${CMAKE_HOST_SYSTEM_NAME}")
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(WARNING "CMAKE_TOOLCHAIN_FILE is not set. Automatically selecting toolchain file...")
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/toolchain/linux-x64.cmake" CACHE PATH "Toolchain file" FORCE)
        message(STATUS "Defaulting to Linux x86_64 toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/toolchain/macos-x64.cmake" CACHE PATH "Toolchain file" FORCE)
        message(STATUS "Defaulting to macOS x86_64 toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/toolchain/windows-x64.cmake" CACHE PATH "Toolchain file" FORCE)
        message(STATUS "Defaulting to Windows x86_64 toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
else()
    message(STATUS "CMake: Using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(STATUS "CMake Target Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    set(Rust_CARGO_TARGET "x86_64-unknown-none")
else()
    message(WARNING "Unsupported target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "CMake Generator: ${CMAKE_GENERATOR}")
message(STATUS "CMake Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CMake Source Directory: ${CMAKE_SOURCE_DIR}")
message(STATUS "CMake Binary Directory: ${CMAKE_BINARY_DIR}")
message(STATUS "CMake Current Source Directory: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CMake Current Binary Directory: ${CMAKE_CURRENT_BINARY_DIR}")
message(STATUS "CMake Host System Name: ${CMAKE_HOST_SYSTEM_NAME}")
message(STATUS "CMake Host System Processor: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message(STATUS "CMake sources configured.")

# ============================================================================
# Standard Configuration
# ============================================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Kernel Linking
# ============================================================================
add_executable(voidframex.krnl
    ${C_SOURCES}
    ${ASM_SOURCES}
)

target_link_libraries(voidframex.krnl PRIVATE linearfb)

# Configure the linker to use ld.lld with proper arguments
set_target_properties(voidframex.krnl PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/voidframex.ld"
)

# Set linker flags for this specific target
target_link_options(voidframex.krnl PRIVATE
    -fuse-ld=lld
    -T ${CMAKE_CURRENT_SOURCE_DIR}/voidframex.ld
    -nostdlib
    -static
    -Wl,-melf_x86_64
)

if (LTO)
    target_link_options(voidframex.krnl PRIVATE
        -flto
        -Wl,--gc-sections
    )
endif()

# ============================================================================
# ISO Generation with Limine Bootloader
# ============================================================================
set(LIMINE_RESOURCE_DIR "/usr/share/limine")

add_custom_command(
        OUTPUT bootd
        COMMAND ${CMAKE_COMMAND} -E touch bootd
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bootdir/kernel
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bootdir/EFI/BOOT
        # Copy Limine UEFI bootloader
        COMMAND ${CMAKE_COMMAND} -E copy ${LIMINE_RESOURCE_DIR}/BOOTX64.EFI
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/EFI/BOOT/BOOTX64.EFI
        # Copy kernel
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:voidframex.krnl>
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/kernel/voidframex.krnl
        # Copy Limine config
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/limine.conf
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/limine.conf
        DEPENDS voidframex.krnl ${CMAKE_CURRENT_SOURCE_DIR}/limine.conf
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Setting up boot directory with Limine"
        VERBATIM
)
add_custom_target(bootdir ALL DEPENDS bootd)

add_custom_command(
        OUTPUT voidframex.iso
        # 1. Copy the specific binary required for UEFI ISO booting
        COMMAND ${CMAKE_COMMAND} -E copy
        ${LIMINE_RESOURCE_DIR}/limine-uefi-cd.bin
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/limine-uefi-cd.bin

        # 2. Run xorriso to build the ISO (UEFI-only per Limine docs)
        COMMAND ${XORRISO} -as mkisofs
        -e limine-uefi-cd.bin           # Use Limine's EFI System Partition image
        -no-emul-boot                   # UEFI El Torito image
        -efi-boot-part                  # Create hidden EFI partition for compatibility
        --efi-boot-image                # Mark as EFI boot image
        --protective-msdos-label        # GPT protective MBR
        -o ${CMAKE_CURRENT_BINARY_DIR}/voidframex.iso
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir

        DEPENDS bootd ${LIMINE_RESOURCE_DIR}/limine-uefi-cd.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating UEFI ISO (voidframex.iso) using xorriso..."
        VERBATIM
)

add_custom_target(iso ALL DEPENDS voidframex.iso)

# ============================================================================
# Run Targets
# ============================================================================
add_custom_target(run
        COMMAND ${QEMU_SYSTEM_X86_64}
        -cpu max
        -no-reboot -no-shutdown
        -m 1G
        -smp 4
        -bios /usr/share/edk2/x64/OVMF.4m.fd
        -debugcon file:bootstrap.log
        -serial stdio
        -boot d
        -cdrom ${CMAKE_CURRENT_BINARY_DIR}/voidframex.iso
        DEPENDS iso
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Running voidframex kernel in QEMU (dev mode)"
)

add_custom_target(img
        COMMAND ${QEMU_IMG} create -f qcow2 VoidFrameDisk.img 16G
        COMMAND ${MKFS_EXT2} VoidFrameDisk.img
        COMMENT "Creating disk images"
)

add_custom_target(extra-img
        COMMAND ${QEMU_IMG} create -f raw VirtioDisk.img 128M
        COMMAND ${MKFS_EXT2} VirtioDisk.img
        COMMAND ${QEMU_IMG} create -f raw SataDisk.img 128M
        COMMAND ${MKFS_EXT2} SataDisk.img
        COMMAND ${QEMU_IMG} create -f raw NVMeDisk.img 256M
        COMMAND ${MKFS_EXT2} NVMeDisk.img
        COMMENT "Creating extra disk images"
)

add_custom_target(dump
        COMMAND ${LLVM_OBJDUMP} -d $<TARGET_FILE:voidframex.krnl> > voidframex.dump
        COMMAND ${LLVM_OBJDUMP} -t $<TARGET_FILE:voidframex.krnl> > voidframex.sym
        DEPENDS voidframex.krnl
        COMMENT "Generating disassembly and symbols"
)
