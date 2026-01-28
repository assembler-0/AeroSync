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
    message(WARNING "Unsupported C compiler: ${CMAKE_C_COMPILER}")
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