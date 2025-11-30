#include <limine/limine.h>
#include <compiler.h>
#include <kernel/panic.h>
#include <drivers/uart/serial.h>
#include <arch/x64/cpu.h>
#include <arch/x64/gdt.h>
#include <lib/printk.h>

#define VOIDFRAMEX_VERSION "0.0.1"
#define VOIDFRAMEX_BUILD_DATE __DATE__ " " __TIME__

// Request framebuffer
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

// Request memory map
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

// Request HHDM (Higher Half Direct Map)
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

// Request RSDP (for ACPI)
__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST_ID,
    .revision = 0
};

void __init __noreturn __noinline __sysv_abi
start_kernel(void) {
    // Ensure we got a framebuffer
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        panic_early();
     }

    // Get the framebuffer
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];

    // Example: Draw something
    for (size_t i = 0; i < 100; i++) {
        uint32_t *fb_ptr = framebuffer->address;
        fb_ptr[i * (framebuffer->pitch / 4) + i] = 0xFFFFFF;
    }

    system_hlt();
    __unreachable();
}
