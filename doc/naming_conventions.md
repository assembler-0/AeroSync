# AeroSync Device Naming Conventions

This document describes the unified naming strategy for devices in the AeroSync kernel, managed by the Unified Driver Model (UDM).

## Overview

AeroSync uses a dynamic, Kconfig-configurable naming system for all hardware devices. This ensures consistency across different subsystems and provides users with a way to customize device names without modifying the source code.

The naming is primarily handled by the UDM Core (`aerosync/sysintf/core/driver_model.c`), which uses `struct class` definitions to determine the naming scheme and prefix for each device type.

## Configurable Prefixes (Kconfig)

The following Kconfig options control the prefixes for various device classes. These can be found under the **"Device Naming Policy"** menu in the driver configuration.

| Subsystem | Kconfig Option | Default Prefix | Example |
| :--- | :--- | :--- | :--- |
| **IDE/PATA** | `CONFIG_IDE_NAME_PREFIX` | `hd` | `hda`, `hdb` |
| **ATAPI/CDROM** | `CONFIG_CDROM_NAME_PREFIX` | `cdrom` | `cdrom0`, `cdrom1` |
| **SATA/SCSI** | `CONFIG_SATA_NAME_PREFIX` | `sd` | `sda`, `sdb` |
| **NVMe** | `CONFIG_NVME_NAME_PREFIX` | `nvme` | `nvme0`, `nvme1` |
| **Interrupt Controllers** | `CONFIG_IC_NAME_PREFIX` | `ic` | `ic0`, `ic1` |
| **Time Sources** | `CONFIG_TIME_NAME_PREFIX` | `timer` | `timer0`, `timer1` |
| **PCI Devices** | `CONFIG_PCI_NAME_PREFIX` | `pci` | `pci_0000:00:01.1` |
| **ACPI Devices** | `CONFIG_ACPI_NAME_PREFIX` | `acpi` | `acpi_LNXP`, `acpi_CPU0` |
| **Serial Ports** | `CONFIG_SERIAL_NAME_PREFIX` | `ttyS` | `ttyS0`, `ttyS1` |
| **Framebuffer** | `CONFIG_FB_NAME_PREFIX` | `fb` | `fb0`, `fb1` |

## Naming Schemes

The UDM supports several naming schemes defined in `include/aerosync/sysintf/class.h`:

1.  **NAMING_NONE:** No automatic naming.
2.  **NAMING_NUMERIC:** Appends a sequence number to the prefix (e.g., `cdrom0`, `timer1`).
3.  **NAMING_ALPHABETIC:** Appends a letter to the prefix (e.g., `hda`, `hdb`, `sda`).
4.  **Custom:** Subsystems can provide their own naming logic before calling `device_add()`, which is used by PCI and ACPI to include bus addresses or namespace IDs.

## Device Tree Visibility

The entire device hierarchy can be logged during boot by enabling `CONFIG_LOG_DEVICE_TREE`. This provides a visual representation of the parent-child relationships between devices (e.g., a block device belonging to an IDE channel on a PCI controller).

Example Log Output:
```
[sys::sysintf::udm] --- Unified Device Tree ---
[sys::sysintf::udm] |- pci_0000:00:01.1 [class: pci, driver: ide]
[sys::sysintf::udm]   |- hda [class: ide, driver: none]
[sys::sysintf::udm]   |- cdrom0 [class: cdrom, driver: none]
[sys::sysintf::udm] |- timer0 [class: time_source, driver: none]
[sys::sysintf::udm] --- End Device Tree ---
```

## Implementation Details

- **Registration:** Devices are registered using `device_register()` or `device_add()`.
- **Naming Helper:** Subsystems call `block_device_assign_name()` or similar helpers to associate a device with its class and trigger the naming logic.
- **Dynamic IDs:** The kernel uses an ID Allocator (`ida`) per class to ensure unique sequence numbers for `NAMING_NUMERIC` and `NAMING_ALPHABETIC` schemes.
