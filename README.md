# AeroSync

**Version:** v1.0.1
**License:** GPL-2.0

<p align="center">
  <img src="https://img.shields.io/github/languages/top/assembler-0/AeroSync?style=for-the-badge&color=blueviolet" />
  <img src="https://img.shields.io/github/repo-size/assembler-0/AeroSync?style=for-the-badge&color=44cc11" />
  <img src="https://img.shields.io/github/license/assembler-0/AeroSync?style=for-the-badge&color=informational" />
  <img src="https://img.shields.io/badge/compiler-Clang/LLVM-9C27B0?style=for-the-badge&logo=llvm" />
</p>

AeroSync is a monolithic, higher-half, 64-bit x86_64 kernel (a KERNEL!, NOT AN OS!) built for modern systems. It is designed with a focus on a clean architecture and draws inspiration from the Linux kernel for many of its subsystems.

## Contributors

- All contributors are listed [here!](CONTRIBUTORS.md)

## Status

| Platform                            | Status                                                                             | Performance |
|:------------------------------------|:-----------------------------------------------------------------------------------|:------------|
| **VMware Workstation Pro 25H2**     | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=vmware)     | High        |
| **QEMU 10.1.2**                     | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=qemu)       | High        |
| **Bochs 3.0.devel**                 | ![](https://img.shields.io/badge/PASSED-brightgreen?style=flat-square&logo=x86)    | Low         |
| **Oracle VirtualBox 7.2.4r170995**  | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=virtualbox) | High        |
| **Intel Alder Lake-based computer** | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=intel)      | High        |

## Features

*   **x86-64 Monolithic Kernel:** A full-featured 64-bit kernel.
*   **Higher-Half Kernel:** The kernel is located in the higher half of the virtual address space.
*   **Preemptive Multitasking:** Allows multiple tasks to run concurrently, with the kernel able to preempt tasks.
*   **Completely Fair Scheduler (CFS):** A modern scheduler implementation inspired by the Linux kernel, ensuring fair CPU time allocation among tasks.
*   **Symmetric Multi-Processing (SMP):** Can utilize multiple CPU cores.
*   **Advanced Memory Management:**
    *   Physical Memory Manager (PMM)
    *   Virtual Memory Manager (VMM) with 4-level paging
    *   Slab Allocator for efficient object caching
    *   Virtual Memory Area (VMA) manager
*   **Virtual File System (VFS):** A Linux-inspired VFS layer that provides a unified interface for file systems. Includes a USTAR (initrd) parser.
*   **Graphical Console:** A framebuffer console provided by the `linearfb` library.
*   **Interrupt Handling:** Supports both modern APIC (x2APIC) and legacy PIC interrupt controllers.
*   **Timers:** Supports HPET and PIT timers.
*   **[FKX](aerosync/fkx/README.md)**: Fused Kernel eXtension for modularity.

## Architecture

AeroSync follows a monolithic design, where all core services run in kernel space. The kernel initialization process (`start_kernel`) provides a clear overview of the architecture:

1.  **Bootloader:** The kernel is loaded by the Limine bootloader.
2.  **Core Initialization:** Basic subsystems like the GDT and IDT are initialized.
3.  **Memory Management:** The PMM, VMM, Slab allocator, and VMA manager are brought online.
4.  **Scheduling:** The Completely Fair Scheduler is initialized.
5.  **Filesystems:** The VFS is initialized, ready to mount filesystems.
6.  **Driver Initialization:** ACPI is parsed via `uACPI`, and essential hardware like interrupt controllers and timers are configured.

The design is heavily influenced by the Linux kernel, adopting concepts like the VFS interface and the CFS scheduler's use of red-black trees.

## Third-Party Projects

AeroSync leverages several excellent open-source projects:

*   **[Limine](https://github.com/limine-bootloader/limine):** A modern, robust bootloader.
*   **[uACPI](https://github.com/uacpi/uacpi):** A lightweight, portable ACPI implementation.
*   **[linearfb](https://github.com/assembler-0/linearfb):** A simple linear framebuffer library.
*   **[Linux](https://github.com/torvalds/linux):** Serves as a major architectural inspiration, with some data structures (like `rbtree.h` and `list.h`) adapted from it.

## Prerequisites

To build AeroSync, you will need the following tools:

*   **CMake:** Version 3.30 or newer
*   **clang** The LLVM C compiler frontend
> ⚠️ AeroSync ONLY supports LLVM-based compiler (eg. clang, icx), compiling with GCC may break the kernel image!
*   **NASM:** The Netwide Assembler
*   **xorriso:** A tool to create and manipulate ISO 9660 images
*   **lld:** The LLVM linker
> ⚠️ LLD is required for LTO builds
*   **Limine:** The Limine bootloader resources must be available on the host system.
> or you can grab limine from its binary branch, make sure to edit CMakeLists.txt for it to use the desired resources

## Building and Running

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/assembler-0/AeroSync.git
    cd AeroSync
    ```

2.  **Configure and build with CMake:**
    ```bash
    cmake -B build
    cmake --build build
    ```

3.  **Run with QEMU:**
    The build process generates a (hybrid) bootable ISO image named `aerosync.hybrid.iso` in the `build/` directory. You can run it with QEMU:
    ```bash
    qemu-system-x86_64 -cdrom build/aerosync.hybrid.iso
    ```

## License

AeroSync is licensed under the **GNU General Public License v2.0**. See the [LICENSE](LICENSE) file for more details.
