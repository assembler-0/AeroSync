# AeroSync Unified Driver Model (UDM)

The AeroSync Unified Driver Model (UDM) is a centralized framework for managing hardware devices, buses, and drivers. It is heavily inspired by the Linux driver model and provides a hierarchical representation of the system's hardware.

## Core Concepts

### 1. `struct device`
The base structure for all hardware devices. Every device in the system (PCI, ACPI, Platform, Block, etc.) must embed or point to a `struct device`.
- **Reference Counting**: Managed via `kref`. Use `get_device()` and `put_device()`.
- **Parent/Child**: Devices form a tree. The parent of a PCI device is the PCI Bus.
- **Bus/Class**: A device belongs to one bus (physical location) and optionally one class (functional type).

### 2. `struct device_driver`
Represents a software driver capable of managing certain devices.
- **Probe**: Called when a potential match is found on the bus.
- **Remove**: Called when the device is detached or the driver is unloaded.
- **Shutdown**: Called during system power-off.

### 3. `struct bus_type`
Represents a physical or logical bus (e.g., PCI, ACPI, Platform).
- **Match**: Logic to determine if a driver can handle a specific device.
- **Probe/Remove**: Bus-level wrappers for driver operations.

### 4. `struct class`
Groups devices by functionality (e.g., `block`, `char`, `tty`, `input`), regardless of how they are connected to the system.

## Usage Guide

### Registering a Driver
Drivers should register with their respective bus during module initialization.

```c
static struct pci_driver my_pci_driver = {
    .name = "my_driver",
    .id_table = my_ids,
    .probe = my_probe,
    .remove = my_remove,
};

int init_module(void) {
    return pci_register_driver(&my_pci_driver);
}
```

### Resource Management
AeroSync is moving towards a managed resource model (`devm_*`). Always prefer these for automatic cleanup.

```c
int my_probe(struct pci_dev *pdev) {
    // Map BAR0 (managed)
    void *mmio = devm_ioremap_resource(&pdev->dev, &pdev->resource[0]);
    if (IS_ERR(mmio)) return PTR_ERR(mmio);
    
    // Request IRQ (managed)
    int irq = pci_get_irq(pdev);
    return devm_request_irq(&pdev->dev, irq, my_handler, 0, "my_device", data);
}
```

## Hierarchy Example
```
[sys::sysintf::udm] --- system device tree ---
[sys::sysintf::udm] |- pci_0000:00:01.1 [class: pci, driver: ide]
[sys::sysintf::udm]   |- hda [class: ide, driver: none]
[sys::sysintf::udm]   |- cdrom0 [class: cdrom, driver: none]
[sys::sysintf::udm] |- acpi_LNXSYSTM:00 [class: acpi, driver: acpi_core]
[sys::sysintf::udm] |- timer0 [class: time_source, driver: none]
```

## Configurability
The UDM is highly configurable via Kconfig under `drivers -> device naming policy`. You can customize:
- Device name prefixes (e.g., `ttyS` vs `com`).
- Automatic `devfs` exposure.
- Verbose topology logging.
