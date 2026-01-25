# AeroSync File Organization

## Overview

AeroSync follows a modular, hierarchical directory structure similar to the Linux kernel. The organization separates different subsystems and components logically to facilitate maintenance and development.

## Directory Structure

```
AeroSync/
├── aerosync/          # Core kernel components
├── arch/              # Architecture-specific code
├── cmake/             # CMake build system components
├── crypto/            # Cryptographic implementations
├── doc/               # Documentation
├── drivers/           # Hardware drivers
├── fs/                # File system implementations
├── include/           # Header files
├── init/              # Kernel initialization
├── kconfig/           # Kconfig build configuration
├── lib/               # Common libraries
├── mm/                # Memory management
├── resources/         # Resource files
├── tools/             # Development tools
├── CMakeLists.txt     # Main build configuration
├── Kconfig           # Kernel configuration
├── README.md         # Project overview
└── ...
```

## Detailed Directory Descriptions

### `aerosync/` - Core Kernel Components
Contains the core kernel functionality:
- **sched/** - Scheduler implementation
- **version.h.in** - Version template file
- Other core kernel modules

### `arch/` - Architecture-Specific Code
Architecture-dependent implementations:
- **x86_64/** - x86_64 specific code
  - Assembly files
  - Boot code
  - Architecture-specific headers
- **Kconfig** - Architecture configuration options

### `cmake/` - Build System Components
CMake build system files:
- **toolchain/** - Toolchain configuration
- **configuration.cmake** - Build configuration
- **dependencies.cmake** - Dependency management
- **kconfig.cmake** - Kconfig integration
- **limine.cmake** - Bootloader integration
- **source.cmake** - Source file management

### `crypto/` - Cryptographic Implementations
Cryptographic algorithms and utilities used by the kernel.

### `doc/` - Documentation
Project documentation, guides, and specifications.

### `drivers/` - Hardware Drivers
Hardware driver implementations:
- **acpi/** - Advanced Configuration and Power Interface drivers
- **apic/** - Advanced Programmable Interrupt Controller drivers
- **pci/** - Peripheral Component Interconnect drivers
- **qemu/** - QEMU-specific drivers
- **timer/** - Timer drivers
- **uart/** - Universal Asynchronous Receiver/Transmitter drivers
- **Kconfig** - Driver configuration options

### `fs/` - File System Implementations
File system drivers and implementations.

### `include/` - Header Files
Public header files organized by subsystem:
- **aerosync/** - Core kernel headers
- **arch/** - Architecture-specific headers
- **crypto/** - Cryptographic headers
- **drivers/** - Driver headers
- **fs/** - File system headers
- **kernel/** - Kernel subsystem headers
- **lib/** - Library headers
- **limine/** - Bootloader headers
- **linux/** - Linux headers
- **mm/** - Memory management headers
- **compiler.h** - Compiler-specific definitions

### `init/` - Kernel Initialization
Kernel startup and initialization code.

### `kconfig/` - Build Configuration
Kconfig files for kernel configuration system:
- **compatability/** – Compatibility options
- **miscellaneous/** - Miscellaneous configuration options

### `lib/` - Common Libraries
Reusable library functions:
- **linearfb/** - Linear framebuffer library
- **uACPI/** - User ACPI library
- Utility functions (bitmap.c, cmdline.c, list.c, etc.)
- **Kconfig** - Library configuration options

### `mm/` - Memory Management
Memory management subsystem implementation.

### `resources/` - Resource Files
Additional resource files used by the kernel.

### `tools/` - Development Tools
Development, debugging, and utility scripts:
- **bochs.conf.in** - Bochs emulator configuration template
- **limine.conf.in** – Limine bootloader configuration template
- **vmware.vmx.in** – VMware VM configuration template

## Key Files

### `CMakeLists.txt`
Main CMake build configuration file that orchestrates the entire build process.

### `Kconfig`
Root Kconfig file that includes all configuration options for the kernel.

### `README.md`
Project overview and quick start guide.

## Coding Standards

The codebase follows Linux kernel-style coding conventions with:
- Consistent indentation and formatting
- Descriptive variable and function names
- Comprehensive commenting for complex algorithms
- Modular design principles
- etc. (I will add more as I go)