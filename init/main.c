#include <arch/x64/cpu.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/smp.h>
#include <compiler.h>
#include <crypto/crc32.h>
#include <drivers/apic/apic.h>
#include <drivers/uart/serial.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <linearfb/font.h>
#include <linearfb/linearfb.h>
#include <arch/x64/mm/pmm.h>

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

void __init __noreturn __noinline __sysv_abi start_kernel(void) {

  const int linear_ret =
      linearfb_init((struct limine_framebuffer_request *)&framebuffer_request);
  if (linear_ret != 0)
    panic_early();
  const int serial_ret = serial_init();
  if (serial_ret != 0)
    if (serial_init_port(COM2) != 0 ||
        serial_init_port(COM3) != 0 ||
        serial_init_port(COM4) != 0
      ) panic_early();

  linearfb_font_t font = {
    .width = 8, .height = 16, .data = (uint8_t *)console_font
  };
  linearfb_load_font(&font, 256);
  linearfb_set_mode(FB_MODE_CONSOLE);
  linearfb_console_clear(0x00000000);
  linearfb_console_set_cursor(0, 0);

  printk_init_auto();

  printk(KERN_CLASS "VoidFrameX (R) v%s - %s\n", VOIDFRAMEX_VERSION,
         VOIDFRAMEX_COMPILER_VERSION);
  printk(KERN_CLASS "copyright (C) 2025 assembler-0\n");
  printk(KERN_CLASS "build: %s\n", VOIDFRAMEX_BUILD_DATE);

  // Initialize Physical Memory Manager
  if (!memmap_request.response || !hhdm_request.response) {
    panic(KERN_CLASS "Memory map or HHDM not available");
  }

  pmm_init(memmap_request.response, hhdm_request.response->offset);
  vmm_init();

  gdt_init();
  idt_install();
  smp_init();
  crc32_init();

  if (!apic_init())
    panic(APIC_CLASS "Failed to initialize APIC");
  apic_timer_init(100);

  printk(KERN_CLASS "Enabling interrupts\n");

  cpu_sti();

  while (1)
    cpu_hlt();

  __unreachable();
}
