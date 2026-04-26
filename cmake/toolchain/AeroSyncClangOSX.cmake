# ============================================================================
# Ceryx Toolchain for LLVM/Clang (Cross-Compilation)
# ============================================================================
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT DEFINED ENV{CLANG_VERSION})
    set(CMAKE_C_COMPILER /usr/local/Cellar/llvm/22.1.4/bin/clang)
    set(CMAKE_CXX_COMPILER /usr/local/Cellar/llvm/22.1.4/bin/clang++)
else()
    set(CMAKE_ENV_CLANG_VERSION "$ENV{CLANG_VERSION}")
    set(CMAKE_C_COMPILER /usr/local/Cellar/llvm/22.1.4/bin/clang-${CMAKE_ENV_CLANG_VERSION})
    set(CMAKE_CXX_COMPILER /usr/local/Cellar/llvm/22.1.4/bin/clang++-${CMAKE_ENV_CLANG_VERSION})
endif()

set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_LINKER /usr/local/Cellar/lld/22.1.4/bin/ld.lld)
set(CMAKE_AR /usr/local/Cellar/llvm/22.1.4/bin/llvm-ar)
set(CMAKE_RANLIB /usr/local/Cellar/llvm/22.1.4/bin/llvm-ranlib)
set(LLVM_STRIP /usr/local/Cellar/llvm/22.1.4/bin/llvm-strip)

set(COMPILER_IDENTIFIER "clang")
set(LINKER_IDENTIFIER "lld")

# Target Triple
if(NOT DEFINED CLANG_TARGET_TRIPLE)
    set(CLANG_TARGET_TRIPLE "x86_64-unknown-none-elf" CACHE STRING "Clang target triple")
endif()

set(CMAKE_C_COMPILER_TARGET ${CLANG_TARGET_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${CLANG_TARGET_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${CLANG_TARGET_TRIPLE})

# Don't look for system headers/libs in host locations
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

