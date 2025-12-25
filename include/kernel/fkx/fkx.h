/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/fkx/fkx.h
 * @brief FKX Module Interface Definitions
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include <kernel/types.h>
#include <drivers/timer/time.h>
#include <drivers/apic/ic.h>
#include <lib/printk.h>
#include <arch/x64/cpu.h>

/* FKX Magic: "FKX1" in little-endian */
#define FKX_MAGIC 0x31584B46

/* FKX Module API Version */
#define FKX_API_VERSION 1

/* Module flags */
#define FKX_FLAG_REQUIRED    (1 << 0)  /* System cannot boot without this module */
#define FKX_FLAG_EARLY_INIT  (1 << 1)  /* Load during early boot phase */
#define FKX_FLAG_CORE        (1 << 2)  /* Core system component */

/* Return codes */
#define FKX_SUCCESS          0
// use errno.h!

typedef enum {
  FKX_PRINTK_CLASS,
  FKX_DRIVER_CLASS,
  FKX_IC_CLASS,
  FKX_MM_CLASS,
  FKX_GENERIC_CLASS,
  FKX_MAX_CLASS,
} fkx_module_class_t;

/* Forward declarations */
struct fkx_kernel_api;
struct fkx_module_info;

/**
 * Module entry point signature
 *
 * @param api Pointer to kernel API table
 * @return FKX_SUCCESS on success, negative error code on failure
 */
typedef int (*fkx_entry_fn)(const struct fkx_kernel_api *api);

/**
 * Kernel API Table
 *
 * Contains function pointers to kernel services available to FKX modules.
 * The kernel guarantees this structure remains valid for the lifetime of
 * the system.
 */
struct fkx_kernel_api {
  uint32_t version; /* API version (FKX_API_VERSION) */
  uint32_t reserved;

  /* Memory management */
  void *   (*kmalloc)(size_t size);
  void     (*kfree)(void *ptr);
  void *   (*vmalloc)(size_t size);
  void *   (*vmalloc_exec)(size_t size);
  void     (*vfree)(void *ptr);
  void *   (*viomap)(uintptr_t phys_addr, size_t size);
  void     (*viounmap)(void *addr);
  int      (*vmm_map_page)(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                         uint64_t flags);
  int      (*vmm_unmap_page)(uint64_t pml4_phys, uint64_t virt);
  uint64_t (*vmm_virt_to_phys)(uint64_t pml4_phys, uint64_t virt);
  void     (*vmm_switch_pml4)(uint64_t pml4_phys);

  /* Memory operations */
  void * (*memset)(void *dest, int c, size_t n);
  void * (*memcpy)(void *dest, const void *src, size_t n);
  void * (*memmove)(void *dest, const void *src, size_t n);
  int    (*memcmp)(const void *s1, const void *s2, size_t n);

  /* String operations */
  int    (*strlen)(const char *s);
  void   (*strcpy)(char *dest, const char *src);
  int    (*strcmp)(const char *s1, const char *s2);

  /* Logging/debug */
  int  (*printk)(const char *fmt, ...);
  int  (*snprintf)(char *buf, size_t size, const char *fmt, ...);
  void (*panic)(const char *msg);

  /* Physical memory */
  uint64_t  (*pmm_alloc_page)(void);
  void      (*pmm_free_page)(uint64_t phys_addr);
  uint64_t  (*pmm_alloc_pages)(size_t count);
  void      (*pmm_free_pages)(uint64_t phys_addr, size_t count);
  void *    (*pmm_phys_to_virt)(uintptr_t phys_addr);
  uintptr_t (*pmm_virt_to_phys)(void *virt_addr);

  /* I/O operations */
  uint8_t  (*inb)(uint16_t port);
  uint16_t (*inw)(uint16_t port);
  uint32_t (*inl)(uint16_t port);
  void (*outb)(uint16_t port, uint8_t value);
  void (*outw)(uint16_t port, uint16_t value);
  void (*outl)(uint16_t port, uint32_t value);

  /* Timing */
  void     (*ndelay)(uint64_t usec);
  void     (*udelay)(uint64_t msec);
  void     (*mdelay)(uint64_t msec);
  void     (*sdelay)(uint64_t sec);
  uint64_t (*get_time_ns)(void);
  uint64_t (*rdtsc)(void);
  void     (*time_register_source)(const time_source_t *source);

  /* Interrupt controlling */
  void     (*ic_register_controller)(const interrupt_controller_interface_t *controller);
  void     (*ic_shutdown_controller)(void);
  void     (*ic_enable_irq)(uint8_t irq_line);
  void     (*ic_disable_irq)(uint8_t irq_line);
  void     (*ic_send_eoi)(uint32_t interrupt_number);
  void     (*ic_set_timer)(uint32_t frequency_hz);
  uint32_t (*ic_get_frequency)(void);
  void     (*ic_send_ipi)(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode);
  interrupt_controller_t (*ic_get_controller_type)(void);

  /* Limine Resources */
  volatile struct limine_framebuffer_request* (*get_framebuffer_request)(void);

  /* printk framework */
  void (*printk_register_backend)(const printk_backend_t *backend);

  int  (*printk_set_sink)(const char *backend_name);
  void (*printk_shutdown)(void);

  /* synchronization */
  void (*spinlock_init)(volatile int *lock);
  void (*spinlock_lock)(volatile int *lock);
  void (*spinlock_unlock)(volatile int *lock);
  irq_flags_t (*spinlock_lock_irqsave)(volatile int *lock);
  void (*spinlock_unlock_irqrestore)(volatile int *lock, irq_flags_t flags);
};

/**
 * Module Information Structure
 *
 * Must be present in every FKX module at a well-known location.
 * Typically placed in a dedicated section (.fkx_info)
 */
struct fkx_module_info {
  uint32_t magic; /* Must be FKX_MAGIC */
  uint32_t api_version; /* FKX_API_VERSION this module was built for */

  const char *name; /* Module name (null-terminated) */
  const char *version; /* Module version string */
  const char *author; /* Author/vendor */
  const char *description; /* Brief description */

  uint32_t flags; /* FKX_FLAG_* combination */
  fkx_module_class_t module_class;

  /* Entry point */
  fkx_entry_fn init;

  /* Dependencies (null-terminated array of module names) */
  const char **depends;

  /* Reserved for future use */
  void *reserved_ptr[4];
};

/**
 * FKX_MODULE_DEFINE - Convenience macro to define module info
 *
 * Usage:
 *   FKX_MODULE_DEFINE(
 *       my_module,
 *       "1.0.0",
 *       "Author Name",
 *       "Module description",
 *       FKX_FLAG_CORE,
 *       my_module_init,
 *       my_module_deps
 *   );
 */
#define FKX_MODULE_DEFINE(_name, ver, auth, desc, flg, cls, entry, deps) \
    __attribute__((section(".fkx_info"), used)) struct fkx_module_info __fkx_module_info_##_name = { \
        .magic = FKX_MAGIC, \
        .api_version = FKX_API_VERSION, \
        .name = #_name, \
        .version = ver, \
        .author = auth, \
        .description = desc, \
        .flags = flg, \
        .module_class = cls, \
        .init = entry, \
        .depends = deps, \
        .reserved_ptr = {0} \
    }

/**
 * FKX_NO_DEPENDENCIES - Use when module has no dependencies
 */

/**
 * Load an FKX module image into memory without calling init
 *
 * @param data Pointer to the start of the ELF module
 * @param size Size of the module in bytes
 * @return FKX_SUCCESS on success, error code otherwise
 */
int fkx_load_image(void *data, size_t size);

/**
 * Initialize all modules of a specific class
 *
 * @param module_class The class of modules to initialize
 * @return FKX_SUCCESS on success, error code otherwise
 */
int fkx_init_module_class(fkx_module_class_t module_class);
