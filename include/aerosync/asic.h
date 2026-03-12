/* SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync Init Chain (ASIC)
 *
 * @file include/aerosync/asic.h
 * @brief Advanced initialization framework for AeroSync.
 * @copyright (C) 2025-2026 assembler-0
 */

#ifndef AEROSYNC_ASIC_H
#define AEROSYNC_ASIC_H

#include <aerosync/types.h>
#include <aerosync/compiler.h>

/**
 * @enum asic_stage
 * @brief Logical phases of the AeroSync boot process.
 */
enum asic_stage {
    ASIC_STAGE_EARLY  = 0x10, /* Pre-MM, early logging/CPU */
    ASIC_STAGE_MM     = 0x20, /* PMM, VMM, SLUB */
    ASIC_STAGE_CORE   = 0x30, /* Sched, RCU, Per-CPU */
    ASIC_STAGE_ARCH   = 0x40, /* GDT/IDT/Syscalls/FPU */
    ASIC_STAGE_FS     = 0x50, /* VFS, ResDomains */
    ASIC_STAGE_BUS    = 0x60, /* ACPI, PCI Discovery */
    ASIC_STAGE_DEVICE = 0x70, /* Drivers (DRM, Timers) */
    ASIC_STAGE_LATE   = 0x80, /* Post-SMP, Kthreads */
    ASIC_STAGE_MAX
};

/**
 * @typedef asic_fn_t
 * @brief Prototype for an initcall function.
 * @return 0 on success, negative error code on failure.
 */
typedef int (*asic_fn_t)(void);

/**
 * @struct asic_descriptor
 * @brief Metadata for a single initcall.
 */
struct asic_descriptor {
    asic_fn_t func;      /* Function pointer */
    const char *name;    /* Human-readable name */
    enum asic_stage stage;      /* Target stage */
    uint32_t priority;   /* Priority (0-99) */
    uint32_t flags;      /* Execution flags (ASIC_FLAG_*) */
};

#define ASIC_FLAG_CRITICAL (1 << 0) /* Panic if this initcall fails */
#define ASIC_FLAG_SMP_ONLY (1 << 1) /* Only run after SMP is initialized */

#define ASIC_PRIORITY_FIRST   00
#define ASIC_PRIORITY_DEFAULT 50
#define ASIC_PRIORITY_LAST    99

/* Internal helper for unique symbol generation and section names */
#define __ASIC_STR(x) #x
#define __ASIC_XSTR(x) __ASIC_STR(x)

/**
 * @brief Internal macro for registering an initcall.
 * @note Uses SORT_BY_NAME in linker script to order by stage and priority.
 */
#define __ASIC_INITCALL(fn, s, prio, f) \
    static const struct asic_descriptor \
    __attribute__((used, section(".asic_data." __ASIC_XSTR(s) "." __ASIC_XSTR(prio) "." __ASIC_XSTR(fn)))) \
    __asic_desc_##fn = { \
        .func = (fn), \
        .name = #fn, \
        .stage = (s), \
        .priority = (prio), \
        .flags = (f), \
    }

/* Public API for registering initcalls */
#define asic_early_init(fn)  __ASIC_INITCALL(fn, ASIC_STAGE_EARLY,  ASIC_PRIORITY_DEFAULT, ASIC_FLAG_CRITICAL)
#define asic_mm_init(fn)     __ASIC_INITCALL(fn, ASIC_STAGE_MM,     ASIC_PRIORITY_DEFAULT, ASIC_FLAG_CRITICAL)
#define asic_core_init(fn)   __ASIC_INITCALL(fn, ASIC_STAGE_CORE,   ASIC_PRIORITY_DEFAULT, ASIC_FLAG_CRITICAL)
#define asic_arch_init(fn)   __ASIC_INITCALL(fn, ASIC_STAGE_ARCH,   ASIC_PRIORITY_DEFAULT, ASIC_FLAG_CRITICAL)
#define asic_fs_init(fn)     __ASIC_INITCALL(fn, ASIC_STAGE_FS,     ASIC_PRIORITY_DEFAULT, 0)
#define asic_bus_init(fn)    __ASIC_INITCALL(fn, ASIC_STAGE_BUS,    ASIC_PRIORITY_DEFAULT, 0)
#define asic_device_init(fn) __ASIC_INITCALL(fn, ASIC_STAGE_DEVICE, ASIC_PRIORITY_DEFAULT, 0)
#define asic_late_init(fn)   __ASIC_INITCALL(fn, ASIC_STAGE_LATE,   ASIC_PRIORITY_DEFAULT, 0)

/* Management Interface */
int asic_run_stage(enum asic_stage stage);
void asic_run_all(void);

#endif /* AEROSYNC_ASIC_H */
