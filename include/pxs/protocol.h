// SPDX License identifier: GPL-2.0-or-later
/**
 * @file protocol.h
 * @brief definitions for the PXS bootloader
 * @version 0.0.1-no-deps
 */

#pragma once

#include <kernel/types.h>

#define PXS_PROTOCOL_VERSION "0.0.1"
#define PXS_MAGIC 0x28082012

typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

typedef struct {
  uint32_t                Type;
  EFI_PHYSICAL_ADDRESS    PhysicalStart;
  EFI_VIRTUAL_ADDRESS     VirtualStart;
  uint32_t                NumberOfPages;
  uint32_t                Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    uint64_t BaseAddress;
    uint64_t Size;
    uint32_t Width;
    uint32_t Height;
    uint32_t PixelsPerScanLine;
    uint8_t  RedMaskSize;
    uint8_t  RedFieldPosition;
    uint8_t  GreenMaskSize;
    uint8_t  GreenFieldPosition;
    uint8_t  BlueMaskSize;
    uint8_t  BlueFieldPosition;
    uint8_t  ReservedMaskSize;
    uint8_t  ReservedFieldPosition;
} PXS_FRAMEBUFFER_INFO;

typedef struct {
    // Header
    uint32_t                Magic;           ///< "PXS!" (0x21535850)
    uint32_t                Version;         ///< Protocol Version
    uint32_t                Flags;           ///< Boot Flags

    // Framebuffer
    PXS_FRAMEBUFFER_INFO    Framebuffer;
    // Memory Map
    EFI_MEMORY_DESCRIPTOR   *MemoryMap;
    unsigned int            MemoryMapSize;
    unsigned int            MapKey;
    unsigned int            DescriptorSize;
    uint32_t                DescriptorVersion;
    // System Tables
    void                    *Rsdp;
    void                    *Smbios;
    uint64_t                RuntimeServicesPtr;
    // Kernel Info
    uint64_t                KernelPhysicalBase;
    uint64_t                KernelFileSize;

    // Modules (Initrd/Ramdisk)
    uint64_t                InitrdAddress;
    uint64_t                InitrdSize;

    // Command Line
    char                    *CommandLine;

    // Security Verification
    uint64_t                SecurityCanary;
} PXS_BOOT_INFO;
