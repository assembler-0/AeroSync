# AeroSync Device Naming & devtmpfs Policy

## Overview
AeroSync maintains a strict separation between kernel mechanism and userspace policy. Unlike Linux, which enforces a rigid Filesystem Hierarchy Standard (FHS), AeroSync allows all device names and mount points to be configured at compile-time or handled dynamically by userspace.

## Configurable Naming
Device names are not hardcoded. The following Kconfig symbols control the default prefixes exposed in `devtmpfs`:

| Device Type | Kconfig Symbol | Default Value | Example |
|-------------|----------------|---------------|---------|
| Serial Port | `CONFIG_SERIAL_NAME_PREFIX` | `ttyS` | `ttyS0`, `ttyS1` |
| Framebuffer | `CONFIG_FB_NAME_PREFIX` | `fb` | `fb0`, `fb1` |
| PTY Slave   | `CONFIG_PTY_SLAVE_PREFIX` | `pts` | `pts0`, `pts1` |
| IDE Disks   | `CONFIG_IDE_NAME_PREFIX` | `hd` | `hda`, `hdb` |
| SATA/SCSI   | `CONFIG_SATA_NAME_PREFIX` | `sd` | `sda`, `sdb` |
| NVMe Disks  | `CONFIG_NVME_NAME_PREFIX` | `nvme` | `nvme0`, `nvme1` |

### Dynamic Indexing
Numbers (suffixes) are allocated dynamically using the kernel's IDA (ID Allocator) or hardware topology indices. 
- **Serial/FB**: Uses a global IDA for each class.
- **Block Devices**:
  - `hd` and `sd` prefixes automatically use alphabetical suffixes (`a`, `b`, ... `aa`, `ab`).
  - Other prefixes (like `nvme`) use numeric suffixes (`0`, `1`).

## devtmpfs
`devtmpfs` is a mountable pseudo-filesystem that drivers use to export their interfaces.
- **Kernel Policy**: The kernel does NOT mount `devtmpfs` to `/dev` by default (unless `CONFIG_DEVTMPFS_MOUNT` is enabled for early debugging).
- **Userspace Policy**: It is the responsibility of the `init` process or a device manager to mount `devtmpfs` at the preferred location.

## Major & Minor Numbers
While AeroSync uses a `major:minor` mapping for character and block devices to maintain architectural sanity, it does NOT strictly follow the Linux Assigned Numbers authority. Drivers are encouraged to use configurable ranges.

### Default Character Assignments
- **Serial (TTY)**: Major 4, Minors starting at 64.
- **Framebuffer**: Major 29, Minors starting at 0.

## Customization
To change the device naming for a custom AeroSync-based distribution:
1. Run `make menuconfig` (or use the CMake `menuconfig` target).
2. Navigate to `drivers` -> `Device Naming Policy`.
3. Update the prefixes as needed.
