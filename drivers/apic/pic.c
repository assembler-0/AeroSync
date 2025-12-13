#include <arch/x64/cpu.h>
#include <arch/x64/io.h>
#include <kernel/types.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_ICW4    0x01
#define ICW1_INIT    0x10
#define ICW4_8086    0x01

uint16_t PIT_FREQUENCY_HZ = 250;
static uint16_t s_irq_mask = 0xFFFF; // All masked initially

void pit_install() {
    const uint16_t divisor = 1193180 / PIT_FREQUENCY_HZ;

    outb(0x43, 0x36);  // Command byte: channel 0, lobyte/hibyte, rate generator
    outb(0x40, divisor & 0xFF);        // Low byte
    outb(0x40, (divisor >> 8) & 0xFF); // High byte
}

void pit_set_frequency(uint16_t hz) {
    // Save current interrupt state
    irq_flags_t flags = save_irq_flags();

    PIT_FREQUENCY_HZ = hz;
    // Safer divisor calculation
    uint32_t div32 = 1193180u / (hz ? hz : 1u);
    uint16_t divisor = (uint16_t)div32;

    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);

    // Restore previous interrupt state
    restore_irq_flags(flags);
}

// Helper to write the cached mask to the PICs
static void pic_write_mask() {
    outb(PIC1_DATA, s_irq_mask & 0xFF);
    outb(PIC2_DATA, (s_irq_mask >> 8) & 0xFF);
}

void pic_enable_irq(uint8_t irq_line) {
    if (irq_line > 15) return;
    s_irq_mask &= ~(1 << irq_line);
    pic_write_mask();
}

void pic_disable_irq(uint8_t irq_line) {
    if (irq_line > 15) return;
    s_irq_mask |= (1 << irq_line);
    pic_write_mask();
}

void pic_send_eoi(uint64_t interrupt_number) {
    if (interrupt_number >= 40) outb(PIC2_COMMAND, 0x20);
    outb(PIC1_COMMAND, 0x20);
}

void pic_install() {
    // Standard initialization sequence
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    // Remap PIC vectors to 0x20-0x2F
    outb(PIC1_DATA, 0x20); // Master PIC vector offset
    
    outb(PIC2_DATA, 0x28); // Slave PIC vector offset
    

    // Configure cascade
    outb(PIC1_DATA, 4); // Tell Master PIC about slave at IRQ2
    
    outb(PIC2_DATA, 2); // Tell Slave PIC its cascade identity
    

    // Set 8086 mode
    outb(PIC1_DATA, ICW4_8086);
    
    outb(PIC2_DATA, ICW4_8086);

    s_irq_mask = 0xFFFF;
    pic_write_mask();
}