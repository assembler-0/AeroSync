# ============================================================================
# ISO Generation with Limine Bootloader
# ============================================================================
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
