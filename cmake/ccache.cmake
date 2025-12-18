# ============================================================================
# CCache Configuration for Faster Builds
# ============================================================================

option(CCACHE "Enable ccache for faster compilation" ON)

if(VCCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        # Configure ccache
        set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})

        # Set ccache options for kernel development
        set(ENV{CCACHE_SLOPPINESS} "file_macro,locale,time_macros")
        set(ENV{CCACHE_MAXSIZE} "12G")
        set(ENV{CCACHE_COMPRESS} "true")
        set(ENV{CCACHE_COMPRESSLEVEL} "6")

        message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")

        # Show ccache stats
        execute_process(
                COMMAND ${CCACHE_PROGRAM} -s
                OUTPUT_VARIABLE CCACHE_STATS
                ERROR_QUIET
        )
        if(CCACHE_STATS)
            message(STATUS "ccache statistics:\n${CCACHE_STATS}")
        endif()
    else()
        message(WARNING "ccache requested but not found")
    endif()
endif()
