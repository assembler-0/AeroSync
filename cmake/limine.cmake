# ============================================================================
# Limine Bootloader Management
# ============================================================================

set(LIMINE_VERSION "v10.x-binary" CACHE STRING "Limine loader version")

# 1. Try to find local Limine (if installed via package manager)
find_program(LIMINE_EXECUTABLE limine)

if(LIMINE_EXECUTABLE)
    # Check version if possible, or just trust the system
    execute_process(COMMAND ${LIMINE_EXECUTABLE} --version
            OUTPUT_VARIABLE LIMINE_SYS_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    message(STATUS "System Limine found: ${LIMINE_EXECUTABLE}")
    
    # We still need the resource files (limine.sys, etc.)
    # Usually in /usr/share/limine
    set(DEFAULT_LIMINE_DIR "/usr/share/limine")
    if(EXISTS "${DEFAULT_LIMINE_DIR}/limine-bios.sys")
        set(LIMINE_RESOURCE_DIR "${DEFAULT_LIMINE_DIR}" CACHE PATH "Path to Limine resource files")
    endif()
endif()

# 2. If resources not found, fetch them
if(NOT DEFINED LIMINE_RESOURCE_DIR OR NOT EXISTS "${LIMINE_RESOURCE_DIR}/limine-bios.sys")
    message(STATUS "Limine resources not found on system. Fetching ${LIMINE_VERSION}...")
    
    FetchContent_Declare(
        limine_binary
        GIT_REPOSITORY https://github.com/limine-bootloader/limine.git
        GIT_TAG        ${LIMINE_VERSION}
    )
    FetchContent_MakeAvailable(limine_binary)
    
    set(LIMINE_RESOURCE_DIR "${limine_binary_SOURCE_DIR}" CACHE PATH "Path to Limine resource files" FORCE)
endif()

message(STATUS "Limine resource directory: ${LIMINE_RESOURCE_DIR}")
