/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file init/main.c
 * @brief Kernel entry point and limine requests
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

#include <arch/x64/cpu.h>
#include <arch/x64/features/features.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/smp.h>
#include <compiler.h>
#include <crypto/crc32.h>
#include <drivers/acpi/power.h>
#include <drivers/apic/apic.h>
#include <drivers/apic/ic.h>
#include <drivers/apic/pic.h>
#include <drivers/qemu/debugcon/debugcon.h>
#include <drivers/timer/hpet.h>
#include <drivers/timer/pit.h>
#include <drivers/timer/time.h>
#include <drivers/uart/serial.h>
#include <fs/vfs.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <kernel/types.h>
#include <kernel/version.h>
#include <lib/linearfb/linearfb.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <uacpi/uacpi.h>
#include <drivers/timer/pit.h>

// Set Limine Request Start Marker
__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

// Set Limine Base Revision to 3
__attribute__((
    used,
    section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = LIMINE_BASE_REVISION(4);

// Request framebuffer
__attribute__((
    used,
    section(".limine_requests"))) volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

// Request memory map
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

// Request paging mode
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_paging_mode_request
    paging_request = {.id = LIMINE_PAGING_MODE_REQUEST_ID,
                      .revision = 0,
                      .mode = LIMINE_PAGING_MODE_X86_64_4LVL};

// Request HHDM (Higher Half Direct Map)
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

// Request RSDP (for ACPI)
__attribute__((used,
               section(".limine_requests"))) volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_smbios_request
    smbios_request = {.id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0};

// Request modules (initrd)
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_module_request
    module_request = {.id = LIMINE_MODULE_REQUEST_ID,
                      .revision = 0}; // New module request

__attribute__((
    used,
    section(".limine_reuests"))) static volatile struct limine_bootloader_info_request 
    bootloader_info_request = {.id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_reuests"))) static volatile struct limine_bootloader_performance_request 
    bootloader_performance_request = {.id = LIMINE_BOOTLOADER_PERFORMANCE_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_executable_cmdline_request 
    cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_firmware_type_request 
    fw_request = {.id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_date_at_boot_request 
    date_at_boot_request = {.id = LIMINE_DATE_AT_BOOT_REQUEST_ID, .revision = 0};

// Set Limine Request End Marker
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static struct task_struct bsp_task;

int kthread_idle(void *data) {
  while (1) {
    cpu_hlt();
  }
}

void __init __noreturn __noinline __sysv_abi start_kernel(void) {
  if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
    panic_early();
  }

  // Register printk backends
  printk_register_backend(debugcon_get_backend());
  printk_register_backend(serial_get_backend());
  printk_register_backend(linearfb_get_backend());

  // check if we recived 4-level paging mode
  if (!paging_request.response ||
      paging_request.response->mode != LIMINE_PAGING_MODE_X86_64_4LVL) {
    panic(KERN_CLASS "4-level paging mode not enabled or paging mode request not found");
  }

  printk_init_auto(NULL);
  pit_calibrate_tsc();

  printk(KERN_CLASS "VoidFrameX (R) v%s - %s\n", VOIDFRAMEX_VERSION,
         VOIDFRAMEX_COMPILER_VERSION);
  printk(KERN_CLASS "copyright (C) 2025 assembler-0\n");
  printk(KERN_CLASS "build: %s\n", VOIDFRAMEX_BUILD_DATE);

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
       bootloader_performance_request.response->init_usec
    );
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
           : "(unknown)"
    );
  }

  if (cmdline_request.response) {
    printk(KERN_CLASS "cmdline: %s\n", cmdline_request.response->cmdline);
  }

  if (date_at_boot_request.response) {
    printk(KERN_INFO "unix timestamp: %lld\n", date_at_boot_request.response->timestamp);
  }

  // Initialize Physical Memory Manager
  if (!memmap_request.response || !hhdm_request.response) {
    panic(KERN_CLASS "memmap/HHDM not available");
  }

  gdt_init();
  idt_install();

  pmm_init(memmap_request.response, hhdm_request.response->offset);

  vmm_init();

  slab_init();

  // Initialize the kernel's virtual memory address space manager
  mm_init(&init_mm);
  init_mm.pml4 = (uint64_t *)pmm_phys_to_virt(g_kernel_pml4);

  // Verify VMA Implementation
  vma_test();


  cpu_features_init();
  // Two-phase ACPI init to break IC/APIC/uACPI circular dependency
  uacpi_kernel_init_early();

  // Register interrupt controllers
  ic_register_controller(apic_get_driver());
  ic_register_controller(pic_get_driver());

  interrupt_controller_t ic_type = ic_install();

  // Notify ACPI glue that IC is ready so it can bind any deferred handlers
  uacpi_notify_ic_ready();

  // Complete ACPI initialization (will install SCI/GPE handlers now that IC is
  // ready)
  uacpi_kernel_init_late();

  // Initialize ACPI Power Management (Buttons, etc.)
  acpi_power_init();

  if (ic_type == INTC_APIC)
    smp_init();
  crc32_init();
  vfs_init();
  if (module_request.response && module_request.response->module_count > 0) {
    struct limine_file *initrd_module = module_request.response->modules[0];
    printk(INITRD_CLASS "Found module '%s' at %p, size %lu\n",
           initrd_module->path, initrd_module->address, initrd_module->size);
  } else {
    printk(INITRD_CLASS "No initrd module found.\n");
  }

  sched_init();
  sched_init_task(&bsp_task);

  // --- Time Subsystem Initialization ---
  printk(KERN_CLASS "Initializing Time Subsystem...\n");

  time_register_source(pit_get_time_source());
  time_register_source(hpet_get_time_source());

  // Initialize unified time subsystem (Selects Best Source and Inits it)
  if (time_init() != 0) {
    printk(KERN_WARNING KERN_CLASS "Time subsystem initialization failed!\n");
  }

  // Recalibrate TSC using the best available time source
  if (time_calibrate_tsc_system() != 0) {
    printk(KERN_WARNING KERN_CLASS "TSC System Calibration failed.\n");
  } else {
    printk(KERN_CLASS "TSC calibrated successfully.\n");
  }

  printk_init_async();

  kthread_run(kthread_create(kthread_idle, NULL, 5, "kthread/idle"));

  printk(KERN_CLASS "Kernel initialization complete, starting init...\n");

  cpu_sti();

  while (1) {
    check_preempt();
    cpu_hlt();
  }

  __unreachable();
}