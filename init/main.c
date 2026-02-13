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
#include <aerosync/sysintf/device.h>
#include <arch/x86_64/tsc.h>
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
#include <aerosync/resdomain.h>
#include <aerosync/sysintf/fw.h>
#include <fs/initramfs.h>
#include <uacpi/uacpi.h>

static alignas(16) struct task_struct bsp_task;

static int __late_init __noreturn __noinline __sysv_abi kernel_init(void *unused) {
  (void) unused;

  printk(KERN_INFO KERN_CLASS "finishing system initialization\n");
  fkx_init_module_class(FKX_GENERIC_CLASS);

  rcu_spawn_kthreads();

#ifdef CONFIG_RCU_PERCPU_TEST
  if (cmdline_get_flag("rcutest")) {
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

  printk(KERN_DEBUG KERN_CLASS "attempting to run init process: %s\n", STRINGIFY(CONFIG_INIT_PATH));
  const int ret = run_init_process(STRINGIFY(CONFIG_INIT_PATH));
  if (ret < 0) {
    printk(KERN_ERR KERN_CLASS "failed to execute %s. (%d)\n", STRINGIFY(CONFIG_INIT_PATH), ret);
    printkln(KERN_ERR KERN_CLASS "attempted to kill init.");
  }

  fw_dump_hardware_info();

  printkln(KERN_CLASS "AeroSync global initialization done.");

  while (1) {
    idle_loop();
  }
}

static int __init __noinline __sysv_abi
system_load_extensions(void) {
  if (lmm_get_count() > 0) {
    printk(KERN_DEBUG FKX_CLASS "Processing modules via LMM...\n");

    lmm_for_each_module(LMM_TYPE_FKX, lmm_load_fkx_callback, nullptr);

    if (fkx_finalize_loading() != 0) {
      printk(KERN_ERR FKX_CLASS "Failed to finalize module loading\n");
      return -ENOSYS;
    }
  } else {
    printk(KERN_NOTICE FKX_CLASS
      "no modules found via LMM"
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
 */
void __no_sanitize __init __noreturn __noinline __sysv_abi start_kernel(void) {
  panic_register_handler(get_builtin_panic_ops());
  panic_handler_install();

  if (LIMINE_BASE_REVISION_SUPPORTED(get_limine_base_revision()) == false) {
    panic_early();
  }

  printk_register_backend(debugcon_get_backend());
  printk_init_early();
  tsc_calibrate_early();

  /* parse cmdline before any stuff get prints */
  if (get_cmdline_request()->response) {
    if (cmdline_find_option_bool(current_cmdline, "quiet")) {
      printk_disable(); /* if 'verbose' is also there, it would still just be logged into the buffer  */
    }
    if (cmdline_find_option_bool(current_cmdline, "verbose")) {
      log_enable_debug();
    }
  }

  printk(KERN_CLASS "AeroSync (R) %s - %s\n", AEROSYNC_VERSION,
         AEROSYNC_COMPILER_VERSION);
  printk(KERN_CLASS "copyright (C) 2025-2026 assembler-0\n");

  if (get_executable_file_request()->response &&
      get_executable_file_request()->response->executable_file) {
    ksymtab_init(get_executable_file_request()->response->executable_file->address);
  }

  if (get_cmdline_request()->response) {
    printkln(KERN_CLASS "cmdline: %s", current_cmdline);
  }

  if (cmdline_find_option_bool(current_cmdline, "bootinfo")) {
    if (
      get_executable_address_request()->response &&
      cmdline_find_option_bool(current_cmdline, "kaslrinfo")
    ) {
      printkln(KERN_CLASS "kaslr base: %p", get_executable_address_request()->response->virtual_base);
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
  }

  printk(KERN_CLASS "system pagination level: %d\n", vmm_get_paging_levels());

  if (get_date_at_boot_request()->response) {
    uint64_t boot_ts = get_date_at_boot_request()->response->timestamp;
    printk(KERN_CLASS "unix timestamp: %lld\n", boot_ts);
    timekeeping_init(boot_ts);
  }

  // Initialize Physical Memory Manager
  if (!get_memmap_request()->response || !get_hhdm_request()->response) {
    panic(KERN_CLASS "memmap/HHDM not available");
  }

  cpu_features_init();
  pmm_init(get_memmap_request()->response, get_hhdm_request()->response->offset,
           get_rsdp_request()->response
             ? get_rsdp_request()->response->address
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

  ksymtab_finalize();

  gdt_init();
  idt_install();
  syscall_init();

  fpu_init();
  pid_allocator_init();
  sched_init();
  bsp_task.active_mm = &init_mm;
  sched_init_task(&bsp_task);

#ifdef CONFIG_LIMINE_MODULE_MANAGER
  lmm_register_prober(initramfs_cpio_prober);
  lmm_register_prober(lmm_fkx_prober);
  lmm_init(get_module_request()->response);
#endif

  vfs_init();
  resdomain_init();

#ifdef INCLUDE_MM_TESTS
  if (cmdline_find_option_bool(current_cmdline, "mtest")) {
    pmm_test();
    vmm_test();
    slab_test();
    vma_test();
    vmalloc_test();
    vmalloc_dump();
    vm_obj_stress_test();
  }
#endif

  fw_init();

  /* load all FKX images */
  system_load_extensions();

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
  time_init();

  // Recalibrate TSC
  time_calibrate_tsc_system();

  timer_init_subsystem();

  // -- initialize the rest of uACPI ---
  uacpi_kernel_init_late();
  acpi_bus_enumerate();
  acpi_power_init();

  fkx_init_module_class(FKX_DRIVER_CLASS);

#ifdef CONFIG_LOG_DEVICE_TREE
  if (cmdline_find_option_bool(current_cmdline, "dumpdevtree"))
    dump_device_tree();
#endif

  if (ic_type == INTC_APIC)
    smp_init();
  crc32_init();
  softirq_init();

#ifdef ASYNC_PRINTK
  printk_init_async();
#endif

  // Start kernel_init thread (do the rest of the kernel init)
  struct task_struct *init_task =
      kthread_create(kernel_init, nullptr, "kernel_init");
  if (!init_task)
    panic(KERN_CLASS "Failed to create kernel_init thread");
  kthread_run(init_task);

  cpu_sti();

  // enter scheduler idle loop (should not reach here)
  idle_loop();

  __unreachable();
}
