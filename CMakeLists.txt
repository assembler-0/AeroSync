# ============================================================================
# SPDX-License-Identifier: GPL-2.0-only
#
# AeroSync monolithic kernel
#
# @file CMakeLists.txt
# @brief Kernel build configuration
# @copyright (C) 2025-2026 assembler-0
#
# This file is part of the AeroSync kernel.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# ============================================================================
cmake_minimum_required(VERSION 3.30)
set_property(GLOBAL PROPERTY RULE_MESSAGES OFF)
project(AeroSync
        VERSION 3.2.5 # <EPOCH>.<FEATURE>.<PATCH> - see include/aerosync/version.h
        LANGUAGES C ASM_NASM
        HOMEPAGE_URL "https://github.com/assembler-0/AeroSync"
        DESCRIPTION "A modern monolithic operating system kernel designed for performance and modularity."
)

# ============================================================================
# fragments
# ============================================================================
list(APPEND CMAKE_MODULE_PATH
        "${CMAKE_SOURCE_DIR}/cmake"
)

# configuration includes
include(cache)
include(ccache)
include(dependencies)
include(configuration)
include(kconfig)
include(platform)
include(standard)
include(version)
include(FetchContent)

# compile sub-projects
add_subdirectory(tools/fkx_signer)

# sources and frameworks
include(source)
include(fkx)

# libraries, drivers configuration
include(lib/linearfb/linearfb.cmake)
include(drivers/uart/uart.cmake)
include(drivers/apic/ic.cmake)
include(drivers/pci/pci.cmake)
include(drivers/timer/timer.cmake)
include(drivers/block/ide/ide.cmake)

# build and post-build
include(aerosync-compile)
include(limine)
include(isogen)
include(run)