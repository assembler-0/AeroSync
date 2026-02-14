# FKX - Fused Kernel eXtension

> "I can't unload this module, it's fused!"

## What is FKX?
FKX is a load-time only kernel extension framework for AeroSync inspired by the linux LKM. Modules are standard ELF shared objects (`ET_DYN`) that are loaded into kernel memory, relocated.

## Why FKX?
- **Load-time only**: Modules are loaded at boot via Limine or specific checkpoints.
- **Permanent**: Once loaded, modules cannot be unloaded.
- **ELF Format**: Modules must be Position Independent Executables (PIE) / Shared Objects.
- **Flexible Loading**: New system allows loading images separately from initialization to avoid circular dependencies.
- **Module Classes**: Modules can be grouped by class for ordered initialization.
- **Signed by default**: For security reasons, all FKX are mandated to be signed by default

## How do i write a module?
Modules should define their metadata using `FKX_MODULE_DEFINE`:

```c
#include <aerosync/fkx/fkx.h>

/*
 * you can use any kernel methods as long as they are exported
 */
#include <lib/printk.h>
#include <mm/vmalloc.h>

static int my_init() {
    void *ptr = vmalloc(8192);
    printk("Hello from FKX!\n");
    vfree(ptr);
    return 0;
}

FKX_MODULE_DEFINE(
    my_mod,            // module name
    "1.0.0",           // module version
    "Author",          // author
    "Description",     // brief description
    0,                 // flags
    FKX_DRIVER_CLASS,  // Module classes
    FKX_SUBCLASS_PCI,  // Subclass (what it provides)
    FKX_NO_REQUIREMENTS, // Requirements (what it needs)
    my_init            // module entry (int (*)(void))
);
```

## How do i compile an FKX module and use it in the AeroSync kernel?
To create an FKX module, you simply have to declare a `[mymod].cmake` as follows
```cmake
file(GLOB_RECURSE MYMOD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_fkx_module(mymod MYMOD_SOURCES)
```
That's basically it!, AeroSync build system will automatically resolve the necessary dependencies and compiles your module correctly and signs it.