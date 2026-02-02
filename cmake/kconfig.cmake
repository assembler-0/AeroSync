# SPDX-License-Identifier: GPL-2.0-only

include_guard(GLOBAL)

find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)

execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import kconfiglib"
        RESULT_VARIABLE _kconfiglib_rc
        OUTPUT_QUIET
        ERROR_QUIET
)
if(NOT _kconfiglib_rc EQUAL 0)
    message(FATAL_ERROR
            "kconfiglib module not found. Install it via 'pip install kconfiglib' or set PYTHONPATH.")
endif()

set(AEROSYNC_KCONFIG "${CMAKE_SOURCE_DIR}/Kconfig" CACHE FILEPATH "Top-level Kconfig file")
set(AEROSYNC_CONFIG "${CMAKE_SOURCE_DIR}/.config" CACHE FILEPATH "Kernel configuration file")
set(AEROSYNC_DEFCONFIG "" CACHE STRING "Initial Kconfig configuration to use")
set(AEROSYNC_FORCE_DEFCONFIG OFF CACHE BOOL "Force re-application of defconfig, overwriting manual changes")

if(AEROSYNC_DEFCONFIG)
    set(_defconfig_path "${CMAKE_SOURCE_DIR}/kconfig/configs/${AEROSYNC_DEFCONFIG}")
    if(EXISTS "${_defconfig_path}")
        set(_last_defconfig "" CACHE INTERNAL "Last applied Kconfig defconfig")

        # Apply defconfig ONLY if .config doesn't exist OR if AEROSYNC_FORCE_DEFCONFIG is ON
        if(NOT EXISTS "${AEROSYNC_CONFIG}" OR AEROSYNC_FORCE_DEFCONFIG)
            message(STATUS "Applying Kconfig defconfig: ${AEROSYNC_DEFCONFIG}")
            configure_file("${_defconfig_path}" "${AEROSYNC_CONFIG}" COPYONLY)
            set(_last_defconfig "${AEROSYNC_DEFCONFIG}" CACHE INTERNAL "Last applied Kconfig defconfig" FORCE)
            set(AEROSYNC_FORCE_DEFCONFIG OFF CACHE BOOL "Force re-application of defconfig, overwriting manual changes" FORCE)
        endif()
    else()
        message(WARNING "Kconfig defconfig '${AEROSYNC_DEFCONFIG}' not found at ${_defconfig_path}")
    endif()
endif()

file(GLOB_RECURSE AEROSYNC_KCONFIG_TREE CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/**/Kconfig")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${AEROSYNC_CONFIG})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${AEROSYNC_KCONFIG})
if(AEROSYNC_KCONFIG_TREE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${AEROSYNC_KCONFIG_TREE})
endif()

set(AEROSYNC_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${AEROSYNC_GENERATED_DIR})
set(AEROSYNC_KCONFIG_CACHE "${AEROSYNC_GENERATED_DIR}/kconfig_cache.cmake")

execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/kconfig_to_cmake.py
        --kconfig ${AEROSYNC_KCONFIG}
        --config ${AEROSYNC_CONFIG}
        --out ${AEROSYNC_KCONFIG_CACHE}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE _kconfig_cache_rc
)
if(NOT _kconfig_cache_rc EQUAL 0)
    message(FATAL_ERROR "Failed to generate Kconfig cache; run menuconfig or inspect the log above.")
endif()

if(EXISTS ${AEROSYNC_KCONFIG_CACHE})
    include(${AEROSYNC_KCONFIG_CACHE})
endif()

set(_kconfig_env "KCONFIG_CONFIG=${AEROSYNC_CONFIG}")

if(DEFINED AEROSYNC_KCONFIG_DEFINES)
    add_compile_definitions(${AEROSYNC_KCONFIG_DEFINES})
endif()

add_custom_target(menuconfig
        COMMAND ${CMAKE_COMMAND} -E env ${_kconfig_env}
        ${Python3_EXECUTABLE} -m menuconfig ${AEROSYNC_KCONFIG}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Launching kconfiglib menuconfig front-end")

add_custom_target(defconfig
        COMMAND ${CMAKE_COMMAND} -E env ${_kconfig_env}
        ${Python3_EXECUTABLE} -m defconfig ${AEROSYNC_KCONFIG}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Launching kconfiglib defconfig front-end")

add_custom_target(oldconfig
        COMMAND ${CMAKE_COMMAND} -E env ${_kconfig_env}
        ${Python3_EXECUTABLE} -m oldconfig ${AEROSYNC_KCONFIG}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Launching kconfiglib oldconfig front-end")

add_custom_target(guiconfig
        COMMAND ${CMAKE_COMMAND} -E env ${_kconfig_env}
        ${Python3_EXECUTABLE} -m guiconfig ${AEROSYNC_KCONFIG}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Launching kconfiglib guiconfig front-end")