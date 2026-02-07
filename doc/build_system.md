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
    - Arch Linux

## Build Process

### Presets (recommended) 

#### Available presets (cmake --list-presets): 
- `amd64` - release kernel (fastest, less overhead, w/o debug symbols)
- `amd64-hardened` - release kernel with hardened security features (moderate performance, w/o debug symbols)
- `amd64-dev` - development kernel (no optimization, w/debug symbols)
- `amd64-dev-hardened` - development kernel with hardened security features (slowest, no optimization, w/debug symbols)

1. **Configure the build:**
   ```bash
   cmake --preset <preset>
   ```
2. **Build the preset:**
   ```bash
   cmake --build --preset <preset>
   ```

> note: if there is already a .config in the project's root, it will use the current config rather than the config correspond to the preset

### Manual configuration (not recommended)

1. **Kconfig setup: (optional)**
    ```bash
   cp kconfig/configs/<your desired config> .config
   ```

2. **Configure the build:**
   ```bash
   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/AeroSyncClang.cmake # -DAEROSYNC_DEFONCONFIG="config here if skip step 1."
   ```

3. **Build:**
   ```bash
   cmake --build build --parallel $(nproc)
   ```

### Build Artifacts

The build process generates:
- `aerosync.hybrid.iso` - A hybrid bootable ISO image in the `build/` directory
- `bootdir/` - Directory containing the kernel image, kernel extensions, and bootloader components
- 
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