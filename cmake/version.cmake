# ============================================================================
# AeroSync versioning system
# ============================================================================
execute_process(
        COMMAND git describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(AEROSYNC_RELEASE "0" CACHE STRING "AeroSync release")
set(AEROSYNC_CANDIDATE "3" CACHE STRING "AeroSync release candidate")
set(AEROSYNC_ABI_LEVEL "0" CACHE STRING "AeroSync ABI level")
set(AEROSYNC_CODENAME "Convergence" CACHE STRING "AeroSync codename")
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/include/aerosync/version.h.in
        ${CMAKE_CURRENT_SOURCE_DIR}/include/aerosync/version.h
        @ONLY
)