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

| Platform                               | Status                                                                             | Performance |
|:---------------------------------------|:-----------------------------------------------------------------------------------|:------------|
| **VMware Workstation Pro 25H2**        | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=vmware)     | High        |
| **QEMU 10.1.2**                        | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=qemu)       | High        |
| **Bochs 3.0.devel**                    | ![](https://img.shields.io/badge/PASSED-brightgreen?style=flat-square&logo=x86)    | Low         |
| **Oracle VirtualBox 7.2.4r170995**     | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=virtualbox) | High        |
| **Intel Alder Lake-based computer**    | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=intel)      | High        |
| **Intel Comet Lake-based computer(s)** | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=intel)      | High        |

## Third-Party Projects

AeroSync leverages several excellent open-source projects:

*   **[Limine](https://github.com/limine-bootloader/limine):** A modern, robust bootloader.
*   **[uACPI](https://github.com/uacpi/uacpi):** A lightweight, portable ACPI implementation.
*   **[linearfb](https://github.com/assembler-0/linearfb):** A simple linear framebuffer library.
*   **[Linux](https://github.com/torvalds/linux):** Serves as a major architectural inspiration, with some data structures (like `rbtree.h` and `list.h`) adapted from it.

## Prerequisites

To build AeroSync, you will need the following tools:

*   **CMake:** Version 3.30 or newer (or, edit the `CMakeLists.txt` file to use an older version)
*   **LLVM**: Version 18.0.0 or newer recommended (including clang, lld, and llvm-*)
> ⚠️ AeroSync ONLY supports LLVM-based compiler (e.g., clang, icx, aocc); GCC is not tested whatsoever
*   **NASM:** The Netwide Assembler
*   **xorriso:** A tool to create and manipulate ISO 9660 images
*   **lld:** The LLVM linker
> ⚠️ LLD is required for LTO builds
*   **Limine:** The Limine bootloader resources must be available on the host system.
*   **Python:** The python interpreter, primarly for tools and kernel configuration generation.
*   **Kconfiglib:** A Python library for parsing Kconfig files.
> CMake will automatically download them if they are not found.

- **optional:** either used by me and/is not needed for building AeroSync
  - mimalloc: glibc's ptmalloc2 sucks
  - llvm-*: replaces GNU equivalents
  - qemu-*: obviously
  - bochs: when I can't use by real computer
  - VMware & VirtualBox: extra fancy
  - ninja: faster than make (theoretically)
  - Arch Linux: I use it
  - mold: will add support (LTO buils are too slow 😭), still fighiting with its linker sctipt support

## Building and Running

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/assembler-0/AeroSync.git
    cd AeroSync
    ```

2.  **Configure and build with CMake:**
    ```bash
    cmake -B build
    cmake --build build --target menuconfig # configure the build, or: python -m *config <SRC_DIR> - choose your favorite editor
    cmake --build build --parallel $(nproc)
    ```

3.  **Aritfacts:**
    The build process generates a (hybrid) bootable ISO image named `aerosync.hybrid.iso` in the `build/` directory. You can run it with QEMU/Bochs/VMware/VirtualBox:
    ```bash
    cmake --build build --target run        # QEMU with UEFI
    cmake --build build --target run-bios   # QEMU with legacy BIOS
    cmake --build build --target bochs-run  # Bochs with BIOS
    cmake --build build --target vmw-run    # WMware with UEFI
    cmake --build build --target vbox-run   # VirtualBox with UEFI
    ```
    There is also `bootdir/` containing the kernel image and the bootloader itself. To run on a real machine, you can either burn the ISO image to a USB drive or copy `bootdir/*` to the root of a FAT32-formatted partition on your machine. 

## License

AeroSync is licensed under the **GNU General Public License v2.0**. See the [LICENSE](LICENSE) file for more details.
