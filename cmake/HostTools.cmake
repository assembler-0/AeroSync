# ============================================================================
# Host Tools Build Configuration
# ============================================================================
# This module builds tools that need to run on the host system (like the signer)
# separately from the kernel, ensuring they use the host's compiler and libraries.

include(ExternalProject)

# 1. Find a Host Compiler
# Since the main project is cross-compiling, we need to explicitly find a host compiler.
# Our toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PROGRAM to NEVER, so this works.
find_program(HOST_C_COMPILER NAMES clang gcc cc)
if(NOT HOST_C_COMPILER)
    message(FATAL_ERROR "No host C compiler found! Needed to build fkx_signer.")
endif()

message(STATUS "Host C Compiler for tools: ${HOST_C_COMPILER}")

# 2. Build fkx_signer
set(FKX_SIGNER_SRC "${CMAKE_SOURCE_DIR}/tools/fkx_signer")
set(FKX_SIGNER_BIN "${CMAKE_BINARY_DIR}/host_tools/fkx_signer")
set(FKX_SIGNER_EXE "${FKX_SIGNER_BIN}/fkx_signer")

ExternalProject_Add(host_fkx_signer
    SOURCE_DIR "${FKX_SIGNER_SRC}"
    BINARY_DIR "${FKX_SIGNER_BIN}"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=${HOST_C_COMPILER}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${FKX_SIGNER_EXE}"
)

# 3. Create Imported Target
add_executable(fkx_signer IMPORTED GLOBAL)
set_target_properties(fkx_signer PROPERTIES
    IMPORTED_LOCATION "${FKX_SIGNER_EXE}"
)

# Ensure the imported target depends on the build step
add_dependencies(fkx_signer host_fkx_signer)
