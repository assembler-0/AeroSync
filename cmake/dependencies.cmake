# ============================================================================
# Tool Dependencies
# ============================================================================
find_program(LLVM_OBJDUMP llvm-objdump)
# find_program(QEMU_IMG qemu-img)
# find_program(MKFS_FAT mkfs.fat)
# find_program(MKFS_EXT2 mkfs.ext2)
# find_program(MKFS_NTFS mkfs.ntfs)
find_program(QEMU_SYSTEM_X86_64 qemu-system-x86_64)
find_program(BOCHS bochs)
find_program(VBOXMANAGE VBoxManage)
#find_program(CARGO_EXECUTABLE cargo REQUIRED)
#find_program(RUSTC_EXECUTABLE rustc REQUIRED)
find_program(XORRISO xorriso REQUIRED)
find_program(VMRUN_EXECUTABLE vmrun)
