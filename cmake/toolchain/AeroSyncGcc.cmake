# ============================================================================
# AeroSync Toolchain for freestanding GCC
# ============================================================================
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-unknown-elf-gcc)
set(CMAKE_CXX_COMPILER x86_64-unknown-elf-g++)
set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_LINKER x86_64-unknown-elf-ld)

set(COMPILER_IDENTIFIER "freestanding-gcc")
set(LINKER_IDENTIFIER "lld")

# Don't look for system headers/libs in host locations
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# Shared library support for Generic platform
set(CMAKE_TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(CMAKE_C_LINK_DYNAMIC_C_FLAGS "-fPIC")
set(CMAKE_SHARED_LIBRARY_C_FLAGS "-fPIC")
set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-shared")
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "-shared")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP "")
set(CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG "")
