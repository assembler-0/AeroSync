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

# ============================================================================
# Project & Toolchain Setup
# ============================================================================

cmake_minimum_required(VERSION 3.30)
project(AeroSync
    VERSION 4.3.7
    LANGUAGES C CXX ASM_NASM
    HOMEPAGE_URL "https://github.com/assembler-0/AeroSync"
    DESCRIPTION "A modern monolithic operating system kernel."
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/toolchain")

# ============================================================================
# Configuration & Dependencies
# ============================================================================

# 1. Base Configuration
include(configuration)  # Build options
include(version)        # Versioning info
include(platform)       # Platform checks
include(standard)       # C/C++ Standards

# 2. Dependencies
include(dependencies)   # External tools (NASM, Xorriso, etc.)
include(limine)         # Bootloader resources

# 3. Kconfig Support
include(kconfig)

# ============================================================================
# Sub-projects (Host Tools)
# ============================================================================

include(HostTools)

# ============================================================================
# Kernel Build
# ============================================================================

# Source Gathering
include(source)       # Core kernel sources
include(fkx)          # FKX framework

# Driver & Library Modules
include(drivers/graphics/drm/linearfb/linearfb.cmake)
include(drivers/uart/uart.cmake)
include(drivers/apic/ic.cmake)
include(drivers/pci/pci.cmake)
include(drivers/timer/timer.cmake)
include(drivers/block/ide/ide.cmake)
include(drivers/char/pty.cmake)
include(drivers/fw/fw.cmake)

# Kernel Target Definition & Compilation Rules
include(aerosync-compile)

# ============================================================================
# Post-Build & artifacts
# ============================================================================
include(isogen) # ISO Generation
include(run)    # Run targets (qemu, etc.)
