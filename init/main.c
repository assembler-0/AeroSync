/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file init/main.c
 * @brief Kernel entry point and initialization
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

#include <arch/x64/fpu.h>
#include <arch/x64/cpu.h>
#include <arch/x64/entry.h>
#include <arch/x64/features/features.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/percpu.h>
#include <arch/x64/smp.h>
#include <compiler.h>
#include <crypto/crc32.h>
#include <drivers/acpi/power.h>
#include <drivers/qemu/debugcon/debugcon.h>
#include <fs/vfs.h>
#include <kernel/classes.h>
#include <kernel/cmdline.h>
#include <kernel/fkx/fkx.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <kernel/sysintf/ic.h>
#include <kernel/sysintf/time.h>
#include <kernel/types.h>
#include <kernel/version.h>
#include <lib/log.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <uacpi/uacpi.h>

// Set Limine Request Start Marker
__attribute__((used,
  section(".limine_requests_start"))) static volatile uint64_t
limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"))) static volatile uint64_t
limine_base_revision[3] = LIMINE_BASE_REVISION(4);

__attribute__((
  used,
  section(".limine_requests"))) volatile struct limine_framebuffer_request
framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((
  used,
  section(".limine_requests"))) static volatile struct limine_memmap_request
memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((
  used,
  section(".limine_requests"))) static volatile struct limine_paging_mode_request
paging_request = {
  .id = LIMINE_PAGING_MODE_REQUEST_ID,
  .revision = 0,
  .mode = LIMINE_PAGING_MODE_X86_64_5LVL
};

__attribute__((
  used,
  section(".limine_requests"))) static volatile struct limine_hhdm_request
hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((used,
  section(".limine_requests"))) volatile struct limine_rsdp_request
rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((
  used,
  section(".limine_requests"))) static volatile struct limine_smbios_request
smbios_request = {.id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0};

__attribute__((
  used,
  section(".limine_requests"))) static volatile struct limine_module_request
module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct
limine_bootloader_info_request bootloader_info_request = {
  .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0
};

__attribute__((used, section(".limine_requests"))) static volatile struct
limine_bootloader_performance_request bootloader_performance_request = {
  .id = LIMINE_BOOTLOADER_PERFORMANCE_REQUEST_ID, .revision = 0
};

__attribute__((used, section(".limine_requests"))) static volatile struct
limine_executable_cmdline_request cmdline_request = {
  .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0
};

__attribute__((used, section(".limine_requests"))) static volatile struct
limine_firmware_type_request fw_request = {
  .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0
};

__attribute__((
  used,
  section(
    ".limine_requests"))) static volatile struct limine_date_at_boot_request
date_at_boot_request = {
  .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
  .revision = 0
};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static struct task_struct bsp_task __aligned(16);

volatile struct limine_framebuffer_request *get_framebuffer_request(void) {
  return &framebuffer_request;
}

EXPORT_SYMBOL(get_framebuffer_request);

static int __init __noreturn __noinline __sysv_abi kernel_init(void *unused) {
  (void) unused;

  printk(KERN_INFO KERN_CLASS "finishing system initialization\n");

  fkx_init_module_class(FKX_GENERIC_CLASS);

  // TODO: Implement run_init_process() which calls do_execve()
  // For now, since we have no init binary on disk, we just stay in kernel
  printk(KERN_NOTICE KERN_CLASS "no init binary found. System idle.\n");

  printk(KERN_CLASS "VoidFrameX initialization complete.\n");

  while (1) {
    check_preempt();
    cpu_hlt();
  }
}

/**
 * @brief VoidFrameX kernel main entry point
 * @note NO RETURN!
 */
void __init __noreturn __noinline __sysv_abi start_kernel(void) {
  panic_register_handler(get_builtin_panic_ops());
  panic_handler_install();

  if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
    panic_early();
  }

  printk_register_backend(debugcon_get_backend());
  printk_init_early();
  tsc_calibrate_early();

  printk(KERN_CLASS "VoidFrameX (R) v%s - %s\n", VOIDFRAMEX_VERSION,
         VOIDFRAMEX_COMPILER_VERSION);
  printk(KERN_CLASS "copyright (C) 2025 assembler-0\n");

  if (bootloader_info_request.response &&
      bootloader_performance_request.response) {
    printk(KERN_CLASS
           "bootloader info: %s %s exec_usec: %llu init_usec: %llu\n",
           bootloader_info_request.response->name
             ? bootloader_info_request.response->name
             : "(null)",
           bootloader_info_request.response->version
             ? bootloader_info_request.response->version
             : "(null-version)",
           bootloader_performance_request.response->exec_usec,
           bootloader_performance_request.response->init_usec);
  }

  if (fw_request.response) {
    printk(FW_CLASS "firmware type: %s\n",
           fw_request.response->firmware_type == LIMINE_FIRMWARE_TYPE_EFI64
             ? "UEFI (64-bit)"
             : fw_request.response->firmware_type == LIMINE_FIRMWARE_TYPE_EFI32
                 ? "UEFI (32-bit)"
                 : fw_request.response->firmware_type == LIMINE_FIRMWARE_TYPE_X86BIOS
                     ? "BIOS (x86)"
                     : fw_request.response->firmware_type == LIMINE_FIRMWARE_TYPE_SBI
                         ? "SBI"
                         : "(unknown)");
  }

  printk(KERN_CLASS "system pagination level: %d\n", vmm_get_paging_levels());

  if (cmdline_request.response) {
    /* Register known options and parse the executable command-line provided by
     * the bootloader (via Limine). Using static storage in the cmdline parser
     * ensures we don't allocate during early boot. */
    cmdline_register_option("verbose", CMDLINE_TYPE_FLAG);
    cmdline_parse(cmdline_request.response->cmdline);
    if (cmdline_verbose()) {
      /* Enable verbose/debug log output which some subsystems consult. */
      log_enable_debug();
      printk(KERN_CLASS "cmdline: verbose enabled\n");
    }
  }

  if (date_at_boot_request.response) {
    printk(KERN_CLASS "unix timestamp: %lld\n",
           date_at_boot_request.response->timestamp);
  }

  // Initialize Physical Memory Manager
  if (!memmap_request.response || !hhdm_request.response) {
    panic(KERN_CLASS "memmap/HHDM not available");
  }

  pmm_init(memmap_request.response, hhdm_request.response->offset);
  vmm_init();
  slab_init();

  setup_per_cpu_areas();
  smp_prepare_boot_cpu();
  pmm_init_cpu();

  gdt_init();
  idt_install();
  syscall_init();

  // MM smoke test
  pmm_test();
  vmm_test();
  slab_test();
  vma_test();
  vmalloc_test();

  if (module_request.response) {
    printk(KERN_DEBUG FKX_CLASS "Found %lu modules, \n",
           module_request.response->module_count);
    for (size_t i = 0; i < module_request.response->module_count; i++) {
      struct limine_file *m = module_request.response->modules[i];
      printk(KERN_DEBUG FKX_CLASS "  [%zu] %s @ %p (%lu bytes)\n", i, m->path,
             m->address, m->size);
      if (fkx_load_image(m->address, m->size) == 0) {
        printk(FKX_CLASS "Successfully loaded module: %s\n", m->path);
      }
    }
    if (fkx_finalize_loading() != 0) {
      printk(KERN_ERR FKX_CLASS "Failed to finalize module loading\n");
    }
  } else {
    printk(KERN_NOTICE FKX_CLASS
      "no FKX module found/loaded"
      ", you probably do not want this"
      ", this build of VoidFrameX does not have "
      "any built-in hardware drivers"
      ", expect exponential lack of hardware support.\n");
  }

  fkx_init_module_class(FKX_PRINTK_CLASS);
  printk_init_late();

  fkx_init_module_class(FKX_IC_CLASS);
  ic_register_lapic_get_id_early();

  cpu_features_init();
  uacpi_kernel_init_early();
  interrupt_controller_t ic_type = ic_install();
  uacpi_notify_ic_ready();
  uacpi_kernel_init_late();
  acpi_power_init();

  // --- Time Subsystem Initialization ---
  fkx_init_module_class(FKX_TIMER_CLASS);
  // Initialize unified time subsystem (Selects Best Source and Inits it)
  if (time_init() != 0) {
    printk(KERN_WARNING KERN_CLASS "Time subsystem initialization failed\n");
  }

  // Recalibrate TSC using the best available time source
  if (time_calibrate_tsc_system() != 0) {
    printk(KERN_WARNING KERN_CLASS "TSC System Calibration failed.\n");
  } else {
    printk(KERN_CLASS "TSC calibrated successfully.\n");
  }

  fkx_init_module_class(FKX_DRIVER_CLASS);

  fpu_init();
  sched_init();
  sched_init_task(&bsp_task);

  if (ic_type == INTC_APIC)
    smp_init();
  crc32_init();
  vfs_init();

  printk_init_async();

  lru_init();
  kswapd_init();
  khugepaged_init();

  // Start kernel_init thread
  struct task_struct *init_task = kthread_create(kernel_init, NULL, "kernel_init");
  if (!init_task)
    panic(KERN_CLASS "Failed to create kernel_init thread");
  kthread_run(init_task);

  cpu_sti();

  // BSP becomes the idle thread
  while (true) {
    check_preempt();
    cpu_hlt();
  }

  __unreachable();
}
