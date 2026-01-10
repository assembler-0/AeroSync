# ============================================================================
# SPDX-License-Identifier: GPL-2.0-only
#
# AeroSync monolithic kernel
#
# @file CMakeLists.txt
# @brief Kernel build configuration
# @copyright (C) 2025 assembler-0
#
# This file is part of the AeroSync kernel.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# ============================================================================
cmake_minimum_required(VERSION 3.30)
set_property(GLOBAL PROPERTY RULE_MESSAGES OFF)
project(AeroSync
        VERSION 2.2.3 # <EPOCH>.<FEATURE>.<PATCH>
        LANGUAGES C CXX ASM_NASM
        HOMEPAGE_URL "https://github.com/assembler-0/AeroSync"
        DESCRIPTION "A modern monolithic operating system kernel designed for performance and modularity."
)
enable_language(ASM_NASM)

execute_process(
        COMMAND git describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(AEROSYNC_RELEASE "0" CACHE STRING "AeroSync release")
set(AEROSYNC_CANDIDATE "1" CACHE STRING "AeroSync release candidate")
set(AEROSYNC_ABI_LEVEL "0" CACHE STRING "AeroSync ABI level")
set(AEROSYNC_CODENAME "Invariant" CACHE STRING "AeroSync codename")
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/include/aerosync/version.h.in
        ${CMAKE_CURRENT_SOURCE_DIR}/include/aerosync/version.h
        @ONLY
)

# ============================================================================
# Module Path & Includes & Subdirectories
# ============================================================================
list(APPEND CMAKE_MODULE_PATH
        "${CMAKE_SOURCE_DIR}/cmake"
)

include(cache)
include(ccache)
include(dependencies)
include(configuration)
include(lib/uACPI/uacpi.cmake)
include(source)
include(fkx)
include(lib/linearfb/linearfb.cmake)
#include(drivers/misc/splash.cmake)
include(drivers/uart/uart.cmake)
include(drivers/apic/ic.cmake)
include(drivers/pci/pci.cmake)
include(drivers/timer/timer.cmake)
include(FetchContent)

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

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(STATUS "CMake Target Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    set(Rust_CARGO_TARGET "x86_64-unknown-none")
else()
    message(WARNING "Unsupported target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_C_COMPILER MATCHES ${ALLOWED_C_COMPILER})
else()
    message(FATAL_ERROR "Unsupported C compiler: ${CMAKE_C_COMPILER}")
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
add_executable(aerosync.krnl
    ${AEROSYNC_SOURCES}
)

target_sources(aerosync.krnl 
	PRIVATE
	${UACPI_SOURCES}
)

target_compile_options(aerosync.krnl PRIVATE 
    $<$<COMPILE_LANGUAGE:C>:
        -m64
        -target ${CLANG_TARGET_TRIPLE}
        -O2
        -g0
        -fdata-sections
        -ffunction-sections
        -fno-omit-frame-pointer
        -finline-functions
        -foptimize-sibling-calls
        -nostdlib
        -ffreestanding
        -mno-implicit-float
        -msoft-float
        -mno-red-zone
        -mserialize
        -fno-pic
        -fno-pie
        -mcmodel=kernel
        -fvisibility=hidden
    >
    $<$<COMPILE_LANGUAGE:ASM_NASM>:
        -felf64
    >
)

if(STACK_PROTECTION)
    target_compile_options(aerosync.krnl PRIVATE 
        $<$<COMPILE_LANGUAGE:C>:
            -fstack-protector-all
            -D_FORTIFY_SOURCE=2
        >
    )
endif()

if(INTEL_CET)
    target_compile_options(aerosync.krnl PRIVATE 
        $<$<COMPILE_LANGUAGE:C>:
            -fcf-protection=full
        >
    )
endif()

if (LTO)
    target_compile_options(aerosync.krnl PRIVATE 
        $<$<COMPILE_LANGUAGE:C>:
            -flto
        >
    )
endif()

if(SANITIZER)
    target_compile_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -fsanitize=undefined,bounds,null,return,vla-bound
        >
    )
endif()

# Configure the linker to use ld.lld with proper arguments
set_target_properties(aerosync.krnl PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/aerosync.ld"
)

# Set linker flags for this specific target
target_link_options(aerosync.krnl PRIVATE
    -fuse-ld=lld
    -T ${CMAKE_CURRENT_SOURCE_DIR}/aerosync.ld
    -nostdlib
    -static
    -Wl,-melf_x86_64
)

if (LTO)
    target_link_options(aerosync.krnl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -flto
            -Wl,--gc-sections
        >
    )
endif()

# ============================================================================
# ISO Generation with Limine Bootloader
# ============================================================================
set(DEFAULT_LIMINE_DIR "/usr/share/limine")
if(EXISTS "${DEFAULT_LIMINE_DIR}")
    set(LIMINE_RESOURCE_DIR "${DEFAULT_LIMINE_DIR}" CACHE STRING "Limine binary directory")
    message(STATUS "Using system Limine: ${LIMINE_RESOURCE_DIR}")
else()
    message(STATUS "Limine not found at ${DEFAULT_LIMINE_DIR}. Fetching from GitHub... (https://github.com/limine-bootloader/limine/tree/v10.x-binary)")

    FetchContent_Declare(
            limine
            GIT_REPOSITORY https://github.com/limine-bootloader/limine.git
            GIT_TAG        v10.x-binary
    )
    FetchContent_MakeAvailable(limine)

    set(LIMINE_RESOURCE_DIR "${limine_SOURCE_DIR}" CACHE STRING "Limine binary directory" FORCE)
    message(STATUS "Limine fetched to: ${LIMINE_RESOURCE_DIR}")
endif()

# Generate limine.conf from template
get_property(FKX_MODULE_LIST GLOBAL PROPERTY FKX_MODULES)
set(LIMINE_MODULES "")
set(FKX_MODULE_FILES "")
foreach(MOD ${FKX_MODULE_LIST})
    string(APPEND LIMINE_MODULES "    module_path: boot():/module/${MOD}.module.fkx\n")
    list(APPEND FKX_MODULE_FILES $<TARGET_FILE:${MOD}>)
endforeach()

configure_file(
    ${CMAKE_SOURCE_DIR}/tools/limine.conf.in
    ${CMAKE_BINARY_DIR}/limine.conf
    @ONLY
)

add_custom_command(
        OUTPUT bootd
        COMMAND ${CMAKE_COMMAND} -E touch bootd
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bootdir/kernel
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bootdir/module
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bootdir/EFI/BOOT
        # Copy Limine UEFI bootloader
        COMMAND ${CMAKE_COMMAND} -E copy ${LIMINE_RESOURCE_DIR}/BOOTX64.EFI
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/EFI/BOOT/BOOTX64.EFI
        # Copy kernel
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:aerosync.krnl>
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/aerosync/aerosync.krnl
        # Copy Limine config
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/limine.conf
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/limine.conf
        DEPENDS aerosync.krnl ${CMAKE_BINARY_DIR}/limine.conf ${FKX_MODULE_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Setting up boot directory with Limine"
        VERBATIM
)

add_custom_target(bootdir ALL DEPENDS bootd)

get_property(FKX_MODULE_LIST GLOBAL PROPERTY FKX_MODULES)
if (FKX_MODULE_LIST)
    add_dependencies(bootdir ${FKX_MODULE_LIST})
endif()

add_custom_command(
    OUTPUT aerosync.hybrid.iso
    # 1. Copy UEFI boot binary
    COMMAND ${CMAKE_COMMAND} -E copy
        ${LIMINE_RESOURCE_DIR}/limine-uefi-cd.bin
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/limine-uefi-cd.bin

    # 2. Copy BIOS boot binary for hybrid support
    COMMAND ${CMAKE_COMMAND} -E copy
        ${LIMINE_RESOURCE_DIR}/limine-bios-cd.bin
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/limine-bios-cd.bin

    COMMAND ${CMAKE_COMMAND} -E copy
        ${LIMINE_RESOURCE_DIR}/limine-bios.sys
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir/limine-bios.sys

    # 3. Create the hybrid ISO with both BIOS and UEFI support
    COMMAND ${XORRISO} -as mkisofs
        -b limine-bios-cd.bin           # BIOS boot (El Torito)
        -no-emul-boot                   # No emulation for BIOS
        -boot-load-size 4               # Load 4 sectors
        -boot-info-table                # Create boot info table for BIOS
        --efi-boot limine-uefi-cd.bin   # UEFI boot
        --efi-boot-part                 # EFI boot partition
        --efi-boot-image                # Mark as EFI bootable
        --protective-msdos-label        # Hybrid MBR
        -o ${CMAKE_CURRENT_BINARY_DIR}/aerosync.hybrid.iso
        ${CMAKE_CURRENT_BINARY_DIR}/bootdir

    DEPENDS bootd $<TARGET_FILE:aerosync.krnl>
        ${LIMINE_RESOURCE_DIR}/limine-uefi-cd.bin
        ${LIMINE_RESOURCE_DIR}/limine-bios.sys
        ${LIMINE_RESOURCE_DIR}/limine-bios-cd.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating hybrid ISO (aerosync.hybrid.iso)..."
    VERBATIM
)

set(ISO_PATH ${CMAKE_CURRENT_BINARY_DIR}/aerosync.hybrid.iso)
add_custom_target(iso ALL DEPENDS aerosync.hybrid.iso)

# Add FKX module dependencies to ISO target
get_property(FKX_MODULE_LIST GLOBAL PROPERTY FKX_MODULES)
if (FKX_MODULE_LIST)
    add_dependencies(iso ${FKX_MODULE_LIST})
endif()

# ============================================================================
# Run Targets
# ============================================================================
if(BOCHS OR QEMU_SYSTEM_X86_64)
    set(ROM_IMAGE "/usr/share/edk2/x64/OVMF.4m.fd" CACHE STRING "UEFI/Custom rom image")
endif()

if (BOCHS)
    set(BOCHS_CONFIG ${CMAKE_CURRENT_BINARY_DIR}/bochs.conf)

    configure_file(
         ${CMAKE_SOURCE_DIR}/tools/bochs.conf.in
         ${BOCHS_CONFIG}
         @ONLY
    )

    add_custom_target(bochs-run
         COMMAND ${BOCHS} -q -f ${BOCHS_CONFIG}
         DEPENDS iso
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
         COMMENT "Running AeroSync kernel in Bochs"
    )
endif()

if(QEMU_SYSTEM_X86_64)
    add_custom_target(run
        COMMAND ${QEMU_SYSTEM_X86_64}
        -M pc,hpet=on
        -cpu max,+la57
        -accel tcg
        -no-reboot -no-shutdown
        -m 4G
        -smp sockets=2,cores=2
        -numa node,nodeid=0,cpus=0-1,memdev=mem0
        -numa node,nodeid=1,cpus=2-3,memdev=mem1
        -object memory-backend-ram,id=mem0,size=2G
        -object memory-backend-ram,id=mem1,size=2G
        -bios ${ROM_IMAGE}
        -debugcon file:bootstrap.log
        -serial stdio
        -boot d
        -cdrom ${ISO_PATH}
        DEPENDS iso
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Running aerosync kernel in QEMU"
    )

    add_custom_target(run-bios
        COMMAND ${QEMU_SYSTEM_X86_64}
        -cpu max
        -no-reboot -no-shutdown
        -m 1G
        -smp 4
        -debugcon file:bootstrap.log
        -serial stdio
        -boot d
        -cdrom ${ISO_PATH}
        DEPENDS iso
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Running aerosync kernel in QEMU"
    )
endif()

if(VBOXMANAGE OR VMRUN)
    set(VM_NAME "AeroSync-${PROJECT_VERSION}" CACHE STRING "VM name for VBox/VMware")
endif()

if(VBOXMANAGE)
    # Target 1: Create the VM
    add_custom_target(vbox-create-vm
            COMMAND ${CMAKE_COMMAND} -E echo "Creating VirtualBox VM: ${VM_NAME}"

        # Delete existing VM if it exists (optional, prevents errors)
        COMMAND ${VBOXMANAGE} unregistervm ${VM_NAME} --delete || true

        # Create new VM
        COMMAND ${VBOXMANAGE} createvm
         --name ${VM_NAME}
         --ostype "Other_64"
         --register

        # Configure VM settings
        COMMAND ${VBOXMANAGE} modifyvm ${VM_NAME}
         --memory 1024
         --vram 16
         --cpus 4
         --boot1 disk
         --hpet on
         --boot2 dvd
         --boot3 none
         --boot4 none
         --acpi on
         --ioapic on
         --rtcuseutc on
         --chipset piix3
         --firmware efi64
         --graphicscontroller vmsvga
         --audio none
         --usb off

        # Add storage controller
        COMMAND ${VBOXMANAGE} storagectl ${VM_NAME}
         --name "IDE Controller"
         --add ide
         --controller PIIX4

        # Attach ISO
        COMMAND ${VBOXMANAGE} storageattach ${VM_NAME}
         --storagectl "IDE Controller"
         --port 0
         --device 0
         --type dvddrive
         --medium ${ISO_PATH}

        # Configure serial port for output
        COMMAND ${VBOXMANAGE} modifyvm ${VM_NAME}
         --uart1 0x3F8 4
         --uartmode1 file ${CMAKE_CURRENT_BINARY_DIR}/vboxserial.log

        COMMAND ${CMAKE_COMMAND} -E echo "VM '${VM_NAME}' created successfully!"
        COMMAND ${CMAKE_COMMAND} -E echo "Serial output will be logged to: ${CMAKE_CURRENT_BINARY_DIR}/serial_output.log"

        COMMENT "Creating VirtualBox VM in BIOS mode"
        VERBATIM
    )

    # Target 3: Launch with GUI (bonus)
    add_custom_target(vbox-run
        COMMAND ${CMAKE_COMMAND} -E echo "Starting VM: ${VM_NAME} (GUI mode)"
        COMMAND ${VBOXMANAGE} startvm ${VM_NAME}

        COMMENT "Running VirtualBox VM with GUI"
        VERBATIM
    )

    # Target 5: Delete the VM
    add_custom_target(vbox-delete-vm
        COMMAND ${CMAKE_COMMAND} -E echo "Deleting VM: ${VM_NAME}"
        COMMAND ${VBOXMANAGE} unregistervm ${VM_NAME} --delete || true

        COMMENT "Deleting VirtualBox VM"
        VERBATIM
    )
endif()

if(VMRUN_EXECUTABLE)
    set(VMWARE_DIR ${CMAKE_BINARY_DIR}/vmware)
    set(VMWARE_VMX ${VMWARE_DIR}/aerosync.vmx)
    set(VMWARE_SERIAL_FILE "${CMAKE_CURRENT_BINARY_DIR}/vmware.serial.log" CACHE STRING "VMware serial output")

    add_custom_target(vmw-setup
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${VMWARE_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VMWARE_DIR}
        COMMAND ${CMAKE_COMMAND}
        -DISO_PATH=${ISO_PATH}
        -DVMWARE_SERIAL_FILE=${VMWARE_SERIAL_FILE}
        -DVM_NAME=${VM_NAME}
        -DVMWARE_VMX=${VMWARE_VMX}
        -P ${CMAKE_SOURCE_DIR}/cmake/vmx.cmake
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
    )

    configure_file(
        ${CMAKE_SOURCE_DIR}/tools/vmware.vmx.in
        ${VMWARE_VMX}
        @ONLY
    )

    add_custom_target(vmw-delete-vm
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${VMWARE_DIR}
        COMMENT "Removing VMware VM"
        VERBATIM
    )

    add_custom_target(vmw-run
        DEPENDS iso

        COMMAND ${VMRUN_EXECUTABLE}
        -T ws
        start ${VMWARE_VMX}

        WORKING_DIRECTORY ${VMWARE_DIR}
        COMMENT "Running AeroSync in VMware"
        VERBATIM
    )
endif()

if(LLVM_OBJDUMP)
    add_custom_target(dump
        COMMAND ${LLVM_OBJDUMP} -d $<TARGET_FILE:aerosync.krnl> > aerosync.dump
        COMMAND ${LLVM_OBJDUMP} -t $<TARGET_FILE:aerosync.krnl> > aerosync.sym
        DEPENDS aerosync.krnl
        COMMENT "Generating disassembly and symbols"
    )
endif()
