# FKX - (initial) Fused Kernel eXtension

> I can't unload this module, it's fixed!

## Overview
- **Load-time only**: Modules are loaded at boot or specific checkpoints.
- **Permanent**: Once loaded, modules cannot be unloaded ("Fused").
- **Secure**: All modules must be signed.
- **ELF Format**: Modules are relocatable ELF objects.

## Structure
The FKX system uses the following structures defined in `fkx.h`:

### `fkx_metadata_t`
Embedded in the module's ELF section `.fkx.meta`. Contains:
- Magic Signature (`FKX1`)
- Module Name
- Version
- Init Entry Point

### `fkx_module_t`
Runtime representation in the kernel. Tracks:
- List node (for global registry)
- Pointer to metadata
- Memory location (Base address, Size)
- Flags (Signed, Active, Broken)
- Signature data

## Usage
Modules define their metadata using the `FKX_DEFINE_MODULE` macro:
```c
FKX_DEFINE_MODULE(my_driver, my_init, 1, 0, 0);
```

## TODO
- [ ] ELF Loader implementation
- [ ] Signature verification logic
- [ ] Symbol resolution (Kernel export table)
