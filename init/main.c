#include "kernel/types.h"
#include <arch/x64/cpu.h>
#include <arch/x64/features/features.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/smp.h>
#include <compiler.h>
#include <crypto/crc32.h>
#include <drivers/apic/ic.h>
#include <drivers/uart/serial.h>
#include <fs/vfs.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <linearfb/font.h>
#include <linearfb/linearfb.h>
#include <mm/slab.h>

#define VOIDFRAMEX_VERSION "0.0.1"
#define VOIDFRAMEX_BUILD_DATE __DATE__ " " __TIME__
#define VOIDFRAMEX_COMPILER_VERSION __VERSION__

// Set Limine Base Revision to 3
__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = LIMINE_BASE_REVISION(3);

// Request framebuffer
__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

// Request memory map
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

// Request HHDM (Higher Half Direct Map)
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

// Request RSDP (for ACPI)
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_smbios_request
    smbios_request = {.id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0};

// Request modules (initrd)
__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_module_request
    module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0}; // New module request

static struct task_struct bsp_task;

int kthread_idle(void *data) {
  while (1) {
    cpu_hlt();
  }
}

void __init __noreturn __noinline __sysv_abi start_kernel(void) {

  const int linear_ret =
      linearfb_init((struct limine_framebuffer_request *)&framebuffer_request);
  if (linear_ret != 0)
    panic_early();
  const int serial_ret = serial_init();
  if (serial_ret != 0)
    if (serial_init_port(COM2) != 0 || serial_init_port(COM3) != 0 ||
        serial_init_port(COM4) != 0)
      panic_early();

  linearfb_font_t font = {
      .width = 8, .height = 16, .data = (uint8_t *)console_font};
  linearfb_load_font(&font, 256);
  linearfb_set_mode(FB_MODE_CONSOLE);
  linearfb_console_clear(0x00000000);
  linearfb_console_set_cursor(0, 0);

  printk_init_auto();

  printk(KERN_CLASS "VoidFrameX (R) v%s - %s\n", VOIDFRAMEX_VERSION,
         VOIDFRAMEX_COMPILER_VERSION);
  printk(KERN_CLASS "copyright (C) 2025 assembler-0\n");
  printk(KERN_CLASS "build: %s\n", VOIDFRAMEX_BUILD_DATE);

  calibrate_tsc();

  // Initialize Physical Memory Manager
  if (!memmap_request.response || !hhdm_request.response) {
    panic(KERN_CLASS "Memory map or HHDM not available");
  }

  gdt_init();
  idt_install();

  pmm_init(memmap_request.response, hhdm_request.response->offset);
  vmm_init();
  slab_init();

  cpu_features_init();
  if (ic_install() == INTC_APIC)
    smp_init();
  crc32_init();
  vfs_init();

  // Check for initrd module and mount it
  if (module_request.response && module_request.response->module_count > 0) {
      // Assuming the first module is our initrd for simplicity
      struct limine_file *initrd_module = module_request.response->modules[0];
      printk(INITRD_CLASS "Found module '%s' at %p, size %lu\n",
             initrd_module->path, initrd_module->address, initrd_module->size);
  } else {
      printk(INITRD_CLASS "No initrd module found.\n");
  }


  sched_init();
  sched_init_task(&bsp_task);

  kthread_run(kthread_create(kthread_idle, NULL, "kthread/idle"));
  kthread_run(kthread_create(kthread_idle, NULL, "kthread/idle"));

  printk(KERN_CLASS "Kernel initialization complete, starting init...\n");

  cpu_sti();

  while (1) {
    check_preempt();
    cpu_hlt();
  }

  __unreachable();
}
