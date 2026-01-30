/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file init/main.c
 * @brief Kernel entry point and initialization
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <aerosync/ksymtab.h>
#include <aerosync/classes.h>
#include <aerosync/cmdline.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/panic.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/softirq.h>
#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/time.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/timer.h>
#include <aerosync/types.h>
#include <aerosync/version.h>
#include <aerosync/rcu.h>
#include <aerosync/percpu.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/entry.h>
#include <arch/x86_64/features/features.h>
#include <arch/x86_64/fpu.h>
#include <arch/x86_64/gdt/gdt.h>
#include <arch/x86_64/idt/idt.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/requests.h>
#include <arch/x86_64/smp.h>
#include <compiler.h>
#include <crypto/crc32.h>
#include <drivers/acpi/power.h>
#include <drivers/qemu/debugcon/debugcon.h>
#include <fs/vfs.h>
#include <lib/log.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <linux/maple_tree.h>
#include <linux/radix-tree.h>
#include <mm/shm.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <mm/zmm.h>
#include <uacpi/uacpi.h>

static alignas(16) struct task_struct bsp_task;

static int __init __noreturn __noinline __sysv_abi kernel_init(void *unused) {
  (void)unused;

  printk(KERN_INFO KERN_CLASS "finishing system initialization\n");
  fkx_init_module_class(FKX_GENERIC_CLASS);

  rcu_spawn_kthreads();

#ifdef CONFIG_RCU_PERCPU_TEST
  if (cmdline_get_flag("verbose")) {
    rcu_test();
    percpu_test();
  }
#endif

  zmm_init();
  shm_init();
  kswapd_init();
  kcompactd_init();
  khugepaged_init();
  vm_writeback_init();
  kvmap_purged_init();

#ifdef MM_HARDENING
  mm_scrubber_init();
#endif

  // TODO: Implement run_init_process() which calls do_execve()
  // For now, since we have no init binary on disk, we just stay in kernel
  printk(KERN_NOTICE KERN_CLASS "no init binary found. System idle.\n");

  printk(KERN_CLASS "AeroSync initialization complete.\n");

  while (1) {
    check_preempt();
    cpu_hlt();
  }
}

static int __init __noinline __sysv_abi
system_load_extensions(const volatile struct limine_module_request *request) {
  if (request->response) {
    printk(KERN_DEBUG FKX_CLASS "Found %lu modules, \n",
           request->response->module_count);
    for (size_t i = 0; i < request->response->module_count; i++) {
      const struct limine_file *m = request->response->modules[i];
      printk(KERN_DEBUG FKX_CLASS "  [%zu] %s @ %p (%lu bytes)\n", i, m->path,
             m->address, m->size);
      if (fkx_load_image(m->address, m->size) == 0) {
        printk(FKX_CLASS "Successfully loaded module: %s\n", m->path);
      }
    }
    if (fkx_finalize_loading() != 0) {
      printk(KERN_ERR FKX_CLASS "Failed to finalize module loading\n");
      return -ENOSYS;
    }
  } else {
    printk(KERN_NOTICE FKX_CLASS
           "no FKX module found/loaded"
           ", you probably do not want this"
           ", this build of AeroSync does not have "
           "any built-in hardware drivers"
           ", expect exponential lack of hardware support.\n");
    return -ENOSYS;
  }
  return 0;
}

/**
 * @brief AeroSync kernel main entry point
 * @note NO RETURN!
 */
void __init __noreturn __noinline __sysv_abi start_kernel(void) {
  panic_register_handler(get_builtin_panic_ops());
  panic_handler_install();

  if (LIMINE_BASE_REVISION_SUPPORTED(get_limine_base_revision()) == false) {
    panic_early();
  }

  printk_register_backend(debugcon_get_backend());
  printk_init_early();
  tsc_calibrate_early();

  printk(KERN_CLASS "AeroSync (R) %s - %s\n", AEROSYNC_VERSION,
         AEROSYNC_COMPILER_VERSION);
  printk(KERN_CLASS "copyright (C) 2025-2026 assembler-0\n");

  if (get_executable_file_request()->response && 
      get_executable_file_request()->response->executable_file) {
      ksymtab_init(get_executable_file_request()->response->executable_file->address);
  }

  if (get_bootloader_info_request()->response &&
      get_bootloader_performance_request()->response) {
    printk(KERN_CLASS
           "bootloader info: %s %s exec_usec: %llu init_usec: %llu\n",
           get_bootloader_info_request()->response->name
               ? get_bootloader_info_request()->response->name
               : "(null)",
           get_bootloader_info_request()->response->version
               ? get_bootloader_info_request()->response->version
               : "(null-version)",
           get_bootloader_performance_request()->response->exec_usec,
           get_bootloader_performance_request()->response->init_usec);
  }

  if (get_fw_request()->response) {
    printk(
        FW_CLASS "firmware type: %s\n",
        get_fw_request()->response->firmware_type == LIMINE_FIRMWARE_TYPE_EFI64
            ? "UEFI (64-bit)"
        : get_fw_request()->response->firmware_type ==
                LIMINE_FIRMWARE_TYPE_EFI32
            ? "UEFI (32-bit)"
        : get_fw_request()->response->firmware_type ==
                LIMINE_FIRMWARE_TYPE_X86BIOS
            ? "BIOS (x86)"
        : get_fw_request()->response->firmware_type == LIMINE_FIRMWARE_TYPE_SBI
            ? "SBI"
            : "(unknown)");
  }

  printk(KERN_CLASS "system pagination level: %d\n", vmm_get_paging_levels());

  if (get_cmdline_request()->response) {
    /* Register known options and parse the executable command-line provided by
     * the bootloader (via Limine). Using static storage in the cmdline parser
     * ensures we don't allocate during early boot. */
    cmdline_register_option("verbose", CMDLINE_TYPE_FLAG);
    cmdline_parse(get_cmdline_request()->response->cmdline);
    if (cmdline_get_flag("verbose")) {
      /* Enable verbose/debug log output which some subsystems consult. */
      log_enable_debug();
      printk(KERN_CLASS "cmdline: verbose enabled\n");
    }
  }

  if (get_date_at_boot_request()->response) {
    printk(KERN_CLASS "unix timestamp: %lld\n",
           get_date_at_boot_request()->response->timestamp);
  }

  // Initialize Physical Memory Manager
  if (!get_memmap_request()->response || !get_hhdm_request()->response) {
    panic(KERN_CLASS "memmap/HHDM not available");
  }

  cpu_features_init();
  pmm_init(get_memmap_request()->response, get_hhdm_request()->response->offset,
           get_rsdp_request()->response ? get_rsdp_request()->response->address
                                        : nullptr);
  lru_init();
  vmm_init();
  slab_init();
  maple_tree_init();
  vma_cache_init();
  radix_tree_init();

  setup_per_cpu_areas();
  rcu_init();

  smp_prepare_boot_cpu();
  pmm_init_cpu();
  vmalloc_init();

  ksymtab_finalize(); // Build optimized symbol index

  gdt_init();
  idt_install();
  syscall_init();

  fpu_init();
  sched_init();
  bsp_task.active_mm = &init_mm;
  sched_init_task(&bsp_task);

#ifdef INCLUDE_MM_TESTS
  if (cmdline_get_flag("verbose")) {
    pmm_test();
    vmm_test();
    slab_test();
    vma_test();
    vmalloc_test();
    vmalloc_dump();
  }
#endif

  system_load_extensions(get_module_request());

  fkx_init_module_class(FKX_PRINTK_CLASS);
  printk_init_late();

  fkx_init_module_class(FKX_IC_CLASS);
  ic_register_lapic_get_id_early();

  uacpi_kernel_init_early();

  acpi_tables_init();
  
  interrupt_controller_t ic_type = ic_install();
  uacpi_notify_ic_ready();

  // --- Time Subsystem Initialization ---
  fkx_init_module_class(FKX_TIMER_CLASS);
  // Initialize unified time subsystem (Selects Best Source and Inits it)
  if (time_init() != 0) {
    printk(KERN_WARNING KERN_CLASS "Time subsystem initialization failed\n");
  }

  // Recalibrate TSC using the best available time source if not already
  // calibrated accurately
  if (tsc_freq_get() < 1000000) {
    if (time_calibrate_tsc_system() != 0) {
      printk(KERN_WARNING KERN_CLASS "TSC System Calibration failed.\n");
    } else {
      printk(KERN_CLASS "TSC calibrated successfully via %s.\n",
             time_get_source_name());
    }
  } else {
    printk(KERN_CLASS "TSC already calibrated via CPUID (%lu Hz).\n",
           tsc_freq_get());
  }

  timer_init_subsystem();

  // -- initialize the rest of uACPI ---
  uacpi_kernel_init_late();
  acpi_bus_enumerate();
  acpi_power_init();

  fkx_init_module_class(FKX_DRIVER_CLASS);

  if (ic_type == INTC_APIC)
    smp_init();
  crc32_init();
  vfs_init();
  softirq_init();

#ifdef ASYNC_PRINTK
  printk_init_async();
#endif

  // Start kernel_init thread
  struct task_struct *init_task =
      kthread_create(kernel_init, nullptr, "kernel_init");
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
