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
include(cache)                        # unorganized cache variables
include(ccache)                       # ccache find & enable
include(dependencies)                 # dependencies
include(configuration)                # build options
include(kconfig)                      # Kconfig-cmake integration
include(version)                      # AeroSync versioning system
include(platform)                     # platform detection
include(standard)                     # Language standards
include(FetchContent)                 # fetcher

# compile sub-projects
add_subdirectory(tools/fkx_signer)    # FKX modules signer

# sources and frameworks
include(source)                       # kernel sources
include(fkx)                          # FKX framework

# libraries, drivers configuration
include(lib/linearfb/linearfb.cmake)  # linearfb graphics library
include(drivers/uart/uart.cmake)      # serial (UART) driver
include(drivers/apic/ic.cmake)        # interrupt controller drivers
include(drivers/pci/pci.cmake)        # pci bus drivers
include(drivers/timer/timer.cmake)    # timer drivers
include(drivers/block/ide/ide.cmake)  # IDE/ATA block driver

# build and post-build
include(aerosync-compile)             # core kernel compilation
include(limine)                       # limine fetcher
include(isogen)                       # iso generation
include(run)                          # runnable targets (menuconfig, run, etc.)