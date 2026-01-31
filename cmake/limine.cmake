# ============================================================================
# Limine fetcher
# ============================================================================

set(DEFAULT_LIMINE_DIR "/usr/share/limine")
if(EXISTS "${DEFAULT_LIMINE_DIR}")
    set(LIMINE_RESOURCE_DIR "${DEFAULT_LIMINE_DIR}" CACHE STRING "Limine binary directory")
    message(STATUS "System Limine found: ${LIMINE_RESOURCE_DIR}")
else()
    message(STATUS "Limine not found at ${DEFAULT_LIMINE_DIR}. Fetching from GitHub... (https://github.com/limine-bootloader/limine/tree/v10.x-binary)")

    FetchContent_Declare(
            limine
            GIT_REPOSITORY https://github.com/limine-bootloader/limine.git
            GIT_TAG        v10.x-binary
    )
    FetchContent_MakeAvailable(limine)

    set(LIMINE_RESOURCE_DIR "${limine_SOURCE_DIR}" CACHE STRING "Limine binary directory" FORCE)
    message(STATUS "Limine fetched to: ${LIMINE_RESOURCE_DIR}")
endif()