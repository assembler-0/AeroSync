#include <arch/x64/cpu.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <arch/x64/smp.h>
#include <compiler.h>
#include <drivers/uart/serial.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <mm/pmm.h>
#include <crypto/crc32.h>

#define VOIDFRAMEX_VERSION "0.0.1"
#define VOIDFRAMEX_BUILD_DATE __DATE__ " " __TIME__
#define VOIDFRAMEX_COMPILER_VERSION __VERSION__

// Set Limine Base Revision to 3
__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = LIMINE_BASE_REVISION(3);

// Request framebuffer
__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_framebuffer_request framebuffer_request =
        {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

// Request memory map
__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_memmap_request memmap_request =
        {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

// Request HHDM (Higher Half Direct Map)
__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_hhdm_request hhdm_request =
        {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

// Request RSDP (for ACPI)
__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_rsdp_request rsdp_request =
        {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_smbios_request smbios_request =
        {.id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0};

void __init __noreturn __noinline __sysv_abi 
start_kernel(void) {

    // Ensure we got a framebuffer
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        panic_early();
    }

    // Serial Initialization
    if (serial_init() != 0)
        if (serial_init_port(COM2) != 0 || serial_init_port(COM3) != 0 ||
            serial_init_port(COM4) != 0)
            panic_early();

    printk_init();

    printk(KERN_CLASS "VoidFrameX (R) v%s - %s\n", VOIDFRAMEX_VERSION, VOIDFRAMEX_COMPILER_VERSION);
    printk(KERN_CLASS "copyright (C) 2025 assembler-0\n");
    printk(KERN_CLASS "build: %s\n", VOIDFRAMEX_BUILD_DATE);

    gdt_init();
    idt_install();
    smp_init();

    // Initialize Physical Memory Manager
    if (!memmap_request.response || !hhdm_request.response) {
        panic(KERN_CLASS "Memory map or HHDM not available");
    }

    pmm_init((void *)memmap_request.response, hhdm_request.response->offset);

    uint64_t page1 = pmm_alloc_page();
    uint64_t page2 = pmm_alloc_page();
    uint64_t pages4 = pmm_alloc_pages(4);

    printk(TEST_CLASS "Allocated: page1=0x%llx, page2=0x%llx, pages4=0x%llx\n",
            page1, page2, pages4);

    // Free them
    pmm_free_page(page1);
    pmm_free_page(page2);
    pmm_free_pages(pages4, 4);
    
    // Get stats
    pmm_stats_t *stats = pmm_get_stats();
    printk(TEST_CLASS "After free: %llu pages free, %llu pages used\n",
            stats->free_pages, stats->used_pages);

    crc32_init();

    printk(KERN_CLASS "VoidFrameX initialization complete... starting init...\n");

    if (framebuffer_request.response &&
        framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *framebuffer =
            framebuffer_request.response->framebuffers[0];
        for (size_t i = 0; i < 100; i++) {
            uint32_t *fb_ptr = framebuffer->address;
            fb_ptr[i * (framebuffer->pitch / 4) + i] = 0xFFFFFF;
        }
    }

    system_hlt();
    __unreachable();
}
