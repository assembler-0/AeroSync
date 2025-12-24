# FKX - Fused Kernel eXtension

> "I can't unload this module, it's fixed!"

## Overview
FKX is a load-time only kernel extension framework for VoidFrameX. Modules are standard ELF shared objects (`ET_DYN`) that are loaded into kernel memory, relocated, and linked via a global kernel API table.

## Features
- **Load-time only**: Modules are loaded at boot via Limine or specific checkpoints.
- **Permanent**: Once loaded, modules cannot be unloaded.
- **ELF Format**: Modules must be Position Independent Executables (PIE) / Shared Objects.
- **API Table**: Kernel exports functionality via `struct fkx_kernel_api`.
- **Flexible Loading**: New system allows loading images separately from initialization to avoid circular dependencies.
- **Module Classes**: Modules can be grouped by class for ordered initialization.

## Structure
- `include/kernel/fkx/fkx.h`: Public API and structures.
- `kernel/fkx/loader.c`: The ELF loader and relocator.
- `kernel/fkx/elf_parser.c`: Internal ELF parsing helpers.

## Creating a Module
Modules should define their metadata using `FKX_MODULE_DEFINE`:

```c
#include <kernel/fkx/fkx.h>

static int my_init(const struct fkx_kernel_api *api) {
    api->printk("Hello from FKX!\n");
    return 0;
}

FKX_MODULE_DEFINE(
    my_mod,
    "1.0.0",
    "Author",
    "Description",
    0,
    FKX_DRIVER_CLASS,  // Module class
    my_init,
    NULL
);
```

## Module Classes
Modules are organized into classes to allow ordered initialization and avoid circular dependencies:

- `FKX_PRINTK_CLASS`: Printk-related modules
- `FKX_DRIVER_CLASS`: Device drivers
- `FKX_IC_CLASS`: Interrupt controller modules
- `FKX_MM_CLASS`: Memory management modules
- `FKX_GENERIC_CLASS`: Generic modules

## New Flexible Loading API

### Loading Images
Use `fkx_load_image()` to load module images without calling initialization:

```c
void *handle;
int ret = fkx_load_image(module_data, module_size, &handle);
if (ret != 0) {
    // Handle error
}
```

### Initializing Module Classes
Initialize all modules of a specific class using `fkx_init_module_class()`:

```c
// Initialize all driver class modules
int ret = fkx_init_module_class(FKX_DRIVER_CLASS);
if (ret != 0) {
    // Handle initialization errors
}
```

### Backwards Compatibility
The old `fkx_load_module()` function is still available for direct load-and-init operations, but is deprecated in favor of the new flexible approach.

## Compilation
Compile modules as freestanding shared objects:
`gcc -shared -fPIC -ffreestanding -nostdlib ...`
