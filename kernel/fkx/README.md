# FKX - Fused Kernel eXtension

> "I can't unload this module, it's fixed!"

## Overview
FKX is a load-time only kernel extension framework for VoidFrameX. Modules are standard ELF shared objects (`ET_DYN`) that are loaded into kernel memory, relocated, and linked via a global kernel API table.

## Features
- **Load-time only**: Modules are loaded at boot via Limine or specific checkpoints.
- **Permanent**: Once loaded, modules cannot be unloaded.
- **ELF Format**: Modules must be Position Independent Executables (PIE) / Shared Objects.
- **API Table**: Kernel exports functionality via `struct fkx_kernel_api`.
- **Dead simple**: No fancy stuff

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
    my_init,
    NULL
);
```

## Compilation
Compile modules as freestanding shared objects:
`gcc -shared -fPIC -ffreestanding -nostdlib ...`
