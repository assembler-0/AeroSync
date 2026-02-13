# AeroSync

**Version:** r0c3-4.3.7

<p align="center">
  <img src="https://img.shields.io/github/languages/top/assembler-0/AeroSync?style=for-the-badge&color=blueviolet" />
  <img src="https://img.shields.io/github/repo-size/assembler-0/AeroSync?style=for-the-badge&color=44cc11" />
  <img src="https://img.shields.io/github/license/assembler-0/AeroSync?style=for-the-badge&color=informational" />
  <img src="https://img.shields.io/badge/compiler-Clang/LLVM-9C27B0?style=for-the-badge&logo=llvm" />
</p>

AeroSync is a (modular) monolithic, amd64 kernel built for modern systems.

## Contributors

- All contributors are listed [here!](CONTRIBUTORS.md), BIG THANKS!

## Status

| Platform                               | Status                                                                             | Performance |
|:---------------------------------------|:-----------------------------------------------------------------------------------|:------------|
| **VMware Workstation Pro 25H2**        | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=vmware)     | High        |
| **QEMU 10.1.2**                        | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=qemu)       | High        |
| **Bochs 3.0.devel**                    | ![](https://img.shields.io/badge/PASSED-brightgreen?style=flat-square&logo=x86)    | Low         |
| **Oracle VirtualBox 7.2.4r170995**     | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=virtualbox) | High        |
| **Intel Alder Lake-based computer**    | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=intel)      | High        |
| **Intel Raptor Lake-based computer**   | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=intel)      | High        |
| **Intel Comet Lake-based computer(s)** | ![](https://img.shields.io/badge/PASSED-success?style=flat-square&logo=intel)      | High        |

## Third-Party Projects

AeroSync leverages several excellent open-source projects:

*   **[Limine](https://github.com/limine-bootloader/limine):** A modern, robust bootloader.
*   **[uACPI](https://github.com/uacpi/uacpi):** A lightweight, portable ACPI implementation.
*   **[linearfb](lib/linearfb/README.md):** A simple linear framebuffer library.
*   **[Linux](https://github.com/torvalds/linux):** Serves as a major architectural inspiration, with 'some' data structures adapted from it.
*   **[XNU](https://github.com/apple-oss-distributions/xnu):** another major architectural inspiration.

## Building and Running (and Prerequisites)

- Please refer to the [build_system.md](doc/build_system.md) for more information.

## Contributing

- Please refer to the [CONTRIBUTING.md](CONTRIBUTING.md) for more information.

## License

AeroSync is licensed under the **GNU General Public License v2.0**. See the [LICENSE](LICENSE) for more details.
