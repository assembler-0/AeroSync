# AeroSync Build System Documentation

## Overview

AeroSync uses CMake as its build system to create a modern, monolithic 64-bit x86_64 kernel. The build system is designed to be modular, supporting various configurations and targets for development and deployment.

## Prerequisites

To build AeroSync, you will need the following tools:
a
*   **CMake:** Version 3.30 or newer (or, edit the `CMakeLists.txt` file to use an older version)
*   **LLVM**: Version 18.0.0 or newer recommended (including clang, lld, and llvm-*)
> ⚠️ AeroSync ONLY supports LLVM-based compiler (e.g., clang, icx, aocc); GCC is not tested whatsoever (don't ask me why)
*   **NASM:** The Netwide Assembler
*   **xorriso:** A tool to create and manipulate ISO 9660 images
*   **lld:** The LLVM linker
> ⚠️ LLD is required for LTO builds
*   **Limine:** The Limine bootloader resources must be available on the host system.
> CMake will automatically download them if they are not found.
*   **Python:** The python interpreter, primarly for tools and kernel configuration generation.
*   **Kconfiglib:** A Python library for parsing Kconfig files.

- **optional:** either used by me and/is not needed for building AeroSync
    - mimalloc: glibc's ptmalloc2 sucks
    - llvm-*: replaces GNU equivalents
    - qemu-*: obviously
    - bochs: when I can't use by real computer
    - VMware & VirtualBox: extra fancy
    - ninja: faster than make (theoretically)
    - Arch Linux: I use it

## Build Process

- `amd64` - release kernel (fastest, less overhead)
- `amd64-hardened` - release kernel with hardened security features
- `amd64-dev` - development kernel with hardened security features (slowest, no optimization and debug symbols)
- `amd64-dev-hardened` - development kernel (no optimization and debug symbols)

1. **Configure the build:**
   ```bash
   cmake --preset <preset>
   ```
2. **Build the preset:**
   ```bash
   cmake --build --preset <preset>
   ```

### Build Artifacts

The build process generates:
- `aerosync.hybrid.iso` - A hybrid bootable ISO image in the `build/` directory
- `bootdir/` - Directory containing the kernel image, kernel extensions, and bootloader components

## CMake components
I want the build system to be as modular as possible, so everything is a CMake fragment located in the `cmake/` directory.
Keeping the root CMakeLists.txt file as clean as possible. Currently, its size is only less than 100 lines of code.

### Compile time options & kernel configuration

There are two ways to configure the kernel:
- **CMake (cache):** `cmake -D<OPTION>=<VALUE> <SRC_DIR>` or `cmake-gui <SRC_DIR>` or `ccmake <SRC_DIR>`
- **Kconfig:** `python -m *config <SRC_DIR>` or `<GENERATOR> *config`

## ISO Generation

The build system creates a hybrid ISO that supports both BIOS and UEFI boot modes using the Limine bootloader:

1. Creates a boot directory structure
2. Copies kernel, bootloader binaries, and configuration
3. Generates a hybrid ISO using xorriso with both BIOS and UEFI boot support

## Development Targets

### Emulator/VM Targets

- `run` - Run in QEMU with UEFI firmware
- `run-bios` - Run in QEMU with legacy BIOS
- `bochs-run` - Run in Bochs emulator
- `vbox-run` - Run in VirtualBox
- `vmw-run` - Run in VMware

### Debugging Targets

- `dump` - Generate disassembly and symbol files
- `menuconfig` - Interactive kernel configuration

### VM Setup Targets

- `vbox-create-vm` - Create VirtualBox VM
- `vbox-delete-vm` - Delete VirtualBox VM
- `vmw-setup` - Setup VMware VM
- `vmw-delete-vm` - Delete VMware VM

## Configuration System

AeroSync uses Kconfig [(more)](kconfig.md) for kernel configuration, allowing fine-grained control over features and options. The configuration system integrates with CMake to generate appropriate build definitions.