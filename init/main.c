#include <pxs/protocol.h>
#include <compiler.h>
#include <kernel/panic.h>
#include <drivers/uart/serial.h>
#include <mm/pmm.h>
#include <arch/x64/cpu.h>
#include <lib/printk.h>

#define VOIDFRAMEX_VERSION "0.0.1"
#define VOIDFRAMEX_BUILD_DATE __DATE__ " " __TIME__

void __init __noreturn __noinline __sysv_abi
start_kernel(PXS_BOOT_INFO* pxs_info) {
    if (pxs_info->Magic != PXS_MAGIC) panic("PXS Magic mismatch\n");
    if (pxs_info->Version != PXS_PROTOCOL_VERSION) panic("PXS Protocol version mismatch\n");

    if (serial_init() != 0)
        if (serial_init_port(COM2) != 0 ||
            serial_init_port(COM3) != 0 ||
            serial_init_port(COM4) != 0
        ) panic("failed to initialize all serial port (COM1-4)\n");

    printk_init();

    printk("VoidFrameX (R) v%s\n", VOIDFRAMEX_VERSION);
    printk("copyright (C) 2025 assembler-0\n");
    printk("build: %s\n", VOIDFRAMEX_BUILD_DATE);

    pmm_init(pxs_info);

    // PMM Test
    void* page = pmm_alloc_page();
    if (page) {
        pmm_free_page(page);
    } else {
        panic("PMM Test: Allocation failed\n");
    }

    // Check if BootInfo and Framebuffer are valid
    if (pxs_info->Framebuffer.BaseAddress) {
        uint32_t *fb = (uint32_t *)pxs_info->Framebuffer.BaseAddress;
        uint64_t pixels = pxs_info->Framebuffer.PixelsPerScanLine * pxs_info->Framebuffer.Height;

        // Construct Red Color based on Mask Positions
        // Assuming 32-bit (8 bit per channel)
        uint32_t red   = 0xFF << pxs_info->Framebuffer.RedFieldPosition;
        uint32_t green = 0x00 << pxs_info->Framebuffer.GreenFieldPosition;
        uint32_t blue  = 0x00 << pxs_info->Framebuffer.BlueFieldPosition;
        uint32_t color = red | green | blue;

        for (uint64_t i = 0; i < pixels; i++) {
            fb[i] = color;
        }
    }

    system_hlt();
}
