# ============================================================================
# Compiler Flags
# ============================================================================
set(C_FLAGS " -m64 -target ${CLANG_TARGET_TRIPLE} -O2 -fno-omit-frame-pointer -finline-functions -foptimize-sibling-calls -nostdlib -fno-builtin -ffreestanding -mno-red-zone -mserialize -fno-pic -fno-pie -mcmodel=kernel -fcf-protection=full -fvisibility=hidden")

set(CMAKE_C_FLAGS "${C_FLAGS}")
set(CMAKE_CXX_FLAGS "${C_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics")

set(ASM_NASM_FLAGS "-f elf64")
set(CMAKE_ASM_NASM_FLAGS "${ASM_NASM_FLAGS}")
