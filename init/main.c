#include <pxs/protocol.h>
#include <compiler.h>
#include <kernel/panic.h>
#include <drivers/uart/serial.h>

void __init __noreturn __noinline __sysv_abi
start_kernel(PXS_BOOT_INFO* BootInfo) {
    if (BootInfo->Magic != PXS_MAGIC) panic("PXS BootInfo Magic is invalid!\n");


    // Check if BootInfo and Framebuffer are valid
    if (BootInfo->Framebuffer.BaseAddress) {
        uint32_t *fb = (uint32_t *)BootInfo->Framebuffer.BaseAddress;
        uint64_t pixels = BootInfo->Framebuffer.PixelsPerScanLine * BootInfo->Framebuffer.Height;

        // Construct Red Color based on Mask Positions
        // Assuming 32-bit (8 bit per channel)
        uint32_t red   = 0xFF << BootInfo->Framebuffer.RedFieldPosition;
        uint32_t green = 0x00 << BootInfo->Framebuffer.GreenFieldPosition;
        uint32_t blue  = 0x00 << BootInfo->Framebuffer.BlueFieldPosition;
        uint32_t color = red | green | blue;

        for (uint64_t i = 0; i < pixels; i++) {
            fb[i] = color;
        }
    }

    system_hlt();
}
