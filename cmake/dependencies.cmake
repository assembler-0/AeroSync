# ============================================================================
# Tool Dependencies & External Packages
# ============================================================================
include(FetchContent)

# ----------------------------------------------------------------------------
# Required Tools
# ----------------------------------------------------------------------------

# Linker (LLD)
find_program(LLD_EXECUTABLE ld.lld)
if(NOT LLD_EXECUTABLE)
    message(WARNING "LLD (ld.lld) not found. Linking might fail or fallback to system ld.")
endif()

# Assembler (NASM)
find_program(NASM_EXECUTABLE nasm REQUIRED)

# ISO Creation (Xorriso)
find_program(XORRISO xorriso REQUIRED)

# Kconfig Dependencies
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if (STRIP)
    find_program(LLVM_STRIP llvm-strip REQUIRED)
endif()


# ----------------------------------------------------------------------------
# Optional Tools (Emulation / Debugging)
# ----------------------------------------------------------------------------
find_program(QEMU_SYSTEM_X86_64 qemu-system-x86_64)
find_program(BOCHS bochs)
find_program(VBOXMANAGE VBoxManage)
find_program(VMRUN_EXECUTABLE vmrun)
find_program(LLVM_OBJDUMP llvm-objdump)

# ----------------------------------------------------------------------------
# External Libraries (FetchContent)
# ----------------------------------------------------------------------------
# Limine is handled in cmake/limine.cmake, but could be moved here.
# For now, we keep the modular structure.