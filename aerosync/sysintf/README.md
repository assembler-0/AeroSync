# AeroSync Unified Driver Model (UDM)

The AeroSync Unified Driver Model is a hierarchical system for managing devices, drivers, buses, and classes. It is heavily inspired by the Linux kernel's driver model and provides a consistent approach to device discovery and lifecycle management.

## Core Concepts

### 1. Devices (`struct device`)
Represents a physical or virtual device in the system. Every device belongs to a **bus** and may belong to a **class**. Devices are reference-counted using `kref`.

### 2. Drivers (`struct device_driver`)
Represents a software component that can manage one or more devices. Drivers are registered with a specific bus.

### 3. Buses (`struct bus_type`)
Represents a communication channel or discovery mechanism (e.g., PCI, USB, Platform, ACPI). The bus is responsible for matching devices to drivers.

### 4. Classes (`struct class`)
A high-level grouping of devices based on their function (e.g., `block`, `char`, `net`, `input`). Classes provide a unified interface for userspace and other kernel subsystems.

## Key Features

### Automatic Naming (IDA)
Classes can define a `dev_name` template (e.g., `ttyS%d`). When a device is added to such a class without a name, the UDM automatically allocates a unique ID using an IDA (ID Allocator) and sets the device name accordingly.

### Hierarchical Topology
Devices can have parents and children, forming a tree that reflects the physical hardware topology.

### Consistent Dispatch
Standardized subsystems like **Block** and **Char** provide common entry points for I/O, ensuring that drivers follow a consistent pattern.

### Module Support (FKX)
The UDM supports dynamic loading and unloading of drivers and subsystems. `bus_unregister` and `class_unregister` ensure that all associated devices and drivers are safely detached and cleaned up.

### Managed Resources (devres)
The UDM provides a managed resource API (`devm_*`) that automatically tracks and releases resources allocated during a driver's `probe` phase. This prevents memory leaks and ensures clean hardware state after driver detachment. Supported managed resources include:
- **Memory**: `devm_kzalloc()`
- **MMIO**: `devm_ioremap()`
- **Interrupts**: `devm_request_irq()`

## Naming Conventions
- **PCI:** `pci{segment}:{bus}:{device}.{function}` (e.g., `0000:00:1f.3`)
- **Block:** `sd[a-z]` for disks, `sd[a-z][1-9]` for partitions.
- **Serial:** `ttyS{index}`
- **Platform:** `{name}.{index}`

## Kconfig Integration
The UDM is highly configurable. Use the following symbols to enable/disable subsystems:
- `CONFIG_SYSINTF`: Core UDM support.
- `CONFIG_UDM_BLOCK`: Block device subsystem.
- `CONFIG_UDM_CHAR`: Character device subsystem.
- `CONFIG_UDM_PCI`: PCI bus support.
- `CONFIG_UDM_ACPI`: ACPI device discovery.