# ============================================================================
# ISO Generation with Limine Bootloader
# ============================================================================
# Generate limine.conf from template
get_property(FKX_MODULE_LIST GLOBAL PROPERTY FKX_MODULES)
set(LIMINE_MODULES "")
set(FKX_MODULE_FILES "")
set(FKX_COPY_COMMANDS "")
foreach(MOD ${FKX_MODULE_LIST})
    string(APPEND LIMINE_MODULES "    module_path: boot():/module/${MOD}.module.fkx\n")
    list(APPEND FKX_MODULE_FILES $<TARGET_FILE:${MOD}>)
    list(APPEND FKX_COPY_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${MOD}> ${CMAKE_CURRENT_BINARY_DIR}/bootdir/module/$<TARGET_FILE_NAME:${MOD}>)
endforeach()

# Handle optional initrd
set(INITRD_COPY_COMMAND "")
set(INITRD_DEPENDENCY "")
set(AEROSYNC_INITRD_CMDLINE "")
if(AEROSYNC_INITRD)
    get_filename_component(INITRD_NAME ${AEROSYNC_INITRD} NAME)
    string(APPEND LIMINE_MODULES "    module_path: boot():/module/${INITRD_NAME}\n")
    set(INITRD_COPY_COMMAND COMMAND ${CMAKE_COMMAND} -E copy ${AEROSYNC_INITRD} ${CMAKE_CURRENT_BINARY_DIR}/bootdir/module/${INITRD_NAME})
    set(INITRD_DEPENDENCY ${AEROSYNC_INITRD})
    set(AEROSYNC_INITRD_CMDLINE "initrd=${INITRD_NAME}")
endif()

configure_file(
        ${CMAKE_SOURCE_DIR}/tools/limine.conf.in
        ${CMAKE_BINARY_DIR}/limine.conf
        @ONLY
)

add_custom_command(
        OUTPUT bootd
        COMMAND ${CMAKE_COMMAND} -E touch bootd
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bootdir/aerosync
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
        # Copy FKX modules
        ${FKX_COPY_COMMANDS}
        # Copy initrd if provided
        ${INITRD_COPY_COMMAND}
        DEPENDS aerosync.krnl ${CMAKE_BINARY_DIR}/limine.conf ${FKX_MODULE_FILES} ${INITRD_DEPENDENCY}
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
