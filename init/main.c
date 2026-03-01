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

#include <aerosync/asrx.h>
#include <aerosync/builtin/panic/panic.h>
#include <aerosync/classes.h>
#include <aerosync/compiler.h>
#include <aerosync/crypto.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/ksymtab.h>
#include <aerosync/panic.h>
#include <aerosync/percpu.h>
#include <aerosync/rcu.h>
#include <aerosync/resdomain.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/softirq.h>
#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/acpica.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/fw.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/sysintf/time.h>
#include <aerosync/timer.h>
#include <aerosync/types.h>
#include <aerosync/version.h>
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
#include <arch/x86_64/tsc.h>
#include <drivers/acpi/power.h>
#include <drivers/qemu/debugcon/debugcon.h>
#include <fs/initramfs.h>
#include <fs/vfs.h>
#include <lib/log.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <linux/maple_tree.h>
#include <linux/radix-tree.h>
#include <mm/ksm.h>
#include <mm/shm.h>
#include <mm/slub.h>
#include <mm/vfs.h>
#include <mm/vm_object.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <mm/zmm.h>

static alignas(16) struct task_struct bsp_task;

static int __late_init __noinline __sysv_abi system_load_extensions(void) {
  if (lmm_get_count() > 0) {
    printk(KERN_DEBUG FKX_CLASS "Processing FKX modules via LMM...\n");

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

static int __late_init __noinline __sysv_abi system_load_modules(void) {
  if (lmm_get_count() > 0) {
    printk(KERN_DEBUG ASRX_CLASS "Processing ASRX modules via LMM...\n");
    lmm_for_each_module(LMM_TYPE_ASRX, lmm_load_asrx_callback, nullptr);
  } else {
    printk(KERN_NOTICE ASRX_CLASS "no modules found via LMM\n");
    return -ENOSYS;
  }
  return 0;
}

static int __late_init __noreturn __noinline __sysv_abi
kernel_init(void *unused) {
  (void) unused;

  printk(KERN_INFO KERN_CLASS "finishing system initialization\n");
  fkx_init_module_class(FKX_GENERIC_CLASS);

  aerosync_core_init(rcu_spawn_kthreads);

#ifdef CONFIG_RCU_PERCPU_TEST
  if (cmdline_get_flag("rcutest")) {
    rcu_test();
    percpu_test();
  }
#endif

  aerosync_core_init(zmm_init);
  aerosync_core_init(shm_init);
  aerosync_core_init(kswapd_init);
  aerosync_core_init(kcompactd_init);
  aerosync_core_init(khugepaged_init);
  aerosync_core_init(vm_writeback_init);
  aerosync_core_init(kvmap_purged_init);
  aerosync_core_init(ksm_init);

#ifdef MM_HARDENING
  if (!cmdline_find_option_bool(current_cmdline, "disable-mm-scrubber"))
    aerosync_core_init(mm_scrubber_init);
#endif

#ifdef CONFIG_VFS_TESTS
  if (cmdline_find_option_bool(current_cmdline, "vfstest"))
    vfs_run_tests();
#endif

  aerosync_extra_init(system_load_modules);

  if (cmdline_find_option_bool(current_cmdline, "fwinfo"))
    fw_dump_hardware_info();

  printk(KERN_CLASS "AeroSync global initialization done.\n");

  char init_path[128];
  cmdline_find_option(current_cmdline, "init", init_path, sizeof(init_path));
  if (init_path[0] == '\0') {
    strcpy(init_path, CONFIG_INIT_PATH);
  }

  printk(KERN_DEBUG KERN_CLASS "attempting to run init process: %s\n",
         init_path);

  const int ret = run_init_process(init_path);
  if (ret < 0)
#ifdef CONFIG_PANIC_ON_INIT_FAIL
    panic(KERN_CLASS "attempted to kill init (%s). (%s)", init_path, errname(ret));
#else
    printk(KERN_ALERT KERN_CLASS "attempted to kill init (%s). (%s)", init_path, errname(ret));
#endif
  else {
    struct task_struct *curr = get_current();
    uint8_t *kstack_top = (uint8_t *) curr->stack + (PAGE_SIZE * 4);
    cpu_regs *regs = (struct cpu_regs *) (kstack_top - sizeof(struct cpu_regs));
    enter_userspace(regs);
  }

  idle_loop();
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
  aerosync_core_init_exprcall(printk_init_early(), printk_early);
  tsc_calibrate_early();

  /* parse cmdline before any stuff get prints */
  if (get_cmdline_request()->response) {
    if (cmdline_find_option_bool(current_cmdline, "quiet")) {
      printk_disable(); /* if 'verbose' is also there, it would still just be
                           logged into the buffer  */
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
    aerosync_core_init(
      ksymtab_init,
      get_executable_file_request()->response->executable_file->address);
  }

  if (get_cmdline_request()->response) {
    printkln(KERN_CLASS "cmdline: %s", current_cmdline);
  } else {
    goto no_cmdline;
  }

  if (cmdline_find_option_bool(current_cmdline, "bootinfo")) {
    if (cmdline_find_option_bool(current_cmdline, "kaslrinfo")) {
      printkln(KERN_CLASS "kaslr base: %p",
               get_executable_address_request()->response->virtual_base);
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
      printk(FW_CLASS "firmware type: %s\n",
             get_fw_request()->response->firmware_type ==
             LIMINE_FIRMWARE_TYPE_EFI64
               ? "UEFI (64-bit)"
               : get_fw_request()->response->firmware_type ==
                 LIMINE_FIRMWARE_TYPE_EFI32
                   ? "UEFI (32-bit)"
                   : get_fw_request()->response->firmware_type ==
                     LIMINE_FIRMWARE_TYPE_X86BIOS
                       ? "BIOS (x86)"
                       : get_fw_request()->response->firmware_type ==
                         LIMINE_FIRMWARE_TYPE_SBI
                           ? "SBI"
                           : "(unknown)");
    }
  }

  if (cmdline_find_option_bool(current_cmdline, "mm_page_lvl")) {
    printk(KERN_CLASS "system pagination level: %d\n", vmm_get_paging_levels());
  }

no_cmdline:

  if (get_date_at_boot_request()->response) {
    uint64_t boot_ts = get_date_at_boot_request()->response->timestamp;
    printk(KERN_CLASS "unix timestamp: %lld\n", boot_ts);
    aerosync_core_init(timekeeping_init, boot_ts);
  }

  unmet_cond_crit(!get_memmap_request()->response ||
    !get_hhdm_request()->response);

  aerosync_core_init(cpu_features_init);
  aerosync_core_init(pmm_init, get_memmap_request()->response,
                     get_hhdm_request()->response->offset,
                     get_rsdp_request()->response
                     ? get_rsdp_request()->response->address
                     : nullptr);
  aerosync_core_init(lru_init);
  aerosync_core_init(vmm_init);
  aerosync_core_init(slab_init);
  aerosync_core_init(maple_tree_init);
  aerosync_core_init(vma_cache_init);
  aerosync_core_init(radix_tree_init);

  aerosync_core_init(setup_per_cpu_areas);
  aerosync_core_init(rcu_init);

  aerosync_core_init(smp_prepare_boot_cpu);
  aerosync_core_init(pmm_init_cpu);
  aerosync_core_init(vmalloc_init);

  aerosync_extra_init(ksymtab_finalize);

  aerosync_core_init(gdt_init);
  aerosync_core_init(idt_install);
  aerosync_core_init(syscall_init);

  aerosync_core_init(fpu_init);
  aerosync_core_init(pid_allocator_init);
  aerosync_core_init(sched_init);
  bsp_task.active_mm = &init_mm;
  bsp_task.cred = &init_cred;
  aerosync_core_init(sched_init_task, &bsp_task);

#ifdef CONFIG_LIMINE_MODULE_MANAGER
  aerosync_core_init(lmm_register_prober, initramfs_cpio_prober);
  aerosync_core_init(lmm_register_prober, lmm_fkx_prober);
  aerosync_core_init(lmm_register_prober, lmm_asrx_prober);
  aerosync_core_init(lmm_init, get_module_request()->response);
#endif

  aerosync_core_init(vfs_init);

  aerosync_core_init(resdomain_init);

  aerosync_core_init(sched_vfs_init);
  aerosync_core_init(mm_vfs_init);

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

  aerosync_core_init(fw_init);
  aerosync_core_init(crypto_init);

  /* load all FKX images */
  aerosync_core_init(system_load_extensions);

  fkx_init_module_class(FKX_PRINTK_CLASS);
  fkx_init_module_class(FKX_PANIC_HANDLER_CLASS);

  aerosync_core_init_exprcall(printk_init_late(), printk_late);
  panic_handler_install();

  fkx_init_module_class(FKX_IC_CLASS);
  aerosync_core_init(ic_register_lapic_get_id_early);

  aerosync_core_init(acpica_kernel_init_early);

  aerosync_core_init(acpi_tables_init);

  interrupt_controller_t ic_type;
  aerosync_core_init_status_ret(ic_install, ic_type);

  acpica_notify_ic_ready();

  // --- Time Subsystem Initialization ---
  fkx_init_module_class(FKX_TIMER_CLASS);
  aerosync_core_init(time_init);

  // Recalibrate TSC
  aerosync_core_init(time_calibrate_tsc_system);

  aerosync_core_init(timer_init_subsystem);

  // -- initialize the rest of ACPI (ACPICA) ---
  aerosync_core_init(acpica_kernel_init_late);
  aerosync_extra_init(acpi_power_init);
  if (cmdline_find_option_bool(current_cmdline, "acpi_enum"))
    aerosync_core_init(acpi_bus_enumerate);

  fkx_init_module_class(FKX_DRIVER_CLASS);

#ifdef CONFIG_LOG_DEVICE_TREE
  if (cmdline_find_option_bool(current_cmdline, "dumpdevtree"))
    dump_device_tree();
#endif

  aerosync_core_init(smp_init, ic_type);
  aerosync_core_init(softirq_init);

#ifdef ASYNC_PRINTK
  aerosync_core_init(printk_init_async);
#endif

  // Start kernel_init thread (do the rest of the kernel init)
  struct task_struct *init_task =
      kthread_create(kernel_init, nullptr, "kernel_init");
  unmet_cond_crit(!init_task);
  kthread_run(init_task);

  cpu_sti();

  // enter scheduler idle loop (should not reach here)
  idle_loop();

  __unreachable();
}
