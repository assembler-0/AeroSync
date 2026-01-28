
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
            -accel kvm
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
