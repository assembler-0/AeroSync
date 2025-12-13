#include <drivers/apic/apic.h>
#include <kernel/classes.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <drivers/apic/ic.h>
#include <drivers/apic/pic.h>

static interrupt_controller_t current_controller = INTC_PIC;
static uint32_t timer_frequency_hz = 100; // default - can be changed at runtime

interrupt_controller_t ic_install(void) {
    printk(IC_CLASS "Initializing interrupt controller...\n");

    if (apic_init()) {
        apic_timer_init(timer_frequency_hz);
        current_controller = INTC_APIC;
        printk(IC_CLASS "APIC detected and initialized\n");
    } else {
        pic_install();
        pit_install();
        pit_set_frequency(timer_frequency_hz);
        current_controller = INTC_PIC;
        printk(IC_CLASS "APIC not available, using PIC\n");
    }
    return current_controller;
}

void ic_enable_irq(uint8_t irq_line) {
    switch (current_controller) {
        case INTC_APIC:
            apic_enable_irq(irq_line);
            break;
        case INTC_PIC:
        default:
            pic_enable_irq(irq_line);
            break;
    }
}

void ic_disable_irq(uint8_t irq_line) {
    switch (current_controller) {
        case INTC_APIC:
            apic_disable_irq(irq_line);
            break;
        case INTC_PIC:
        default:
            pic_disable_irq(irq_line);
            break;
    }
}

void ic_send_eoi(uint64_t interrupt_number) {
    switch (current_controller) {
        case INTC_APIC:
            apic_send_eoi();
            break;
        case INTC_PIC:
        default:
            pic_send_eoi(interrupt_number);
            break;
    }
}

interrupt_controller_t ic_get_controller_type(void) {
    return current_controller;
}

const char* ic_get_controller_name(void) {
    switch (current_controller) {
        case INTC_APIC:
            return "APIC";
        case INTC_PIC:
        default:
            return "PIC";
    }
}

void ic_set_timer(const uint32_t frequency_hz) {
    switch (current_controller) {
        case INTC_APIC:
            apic_timer_set_frequency(frequency_hz);
            break;
        case INTC_PIC:
        default:
            pit_set_frequency(frequency_hz > 65535 ? 65535 : frequency_hz);
            break;
    }
    timer_frequency_hz = frequency_hz;
}

uint32_t ic_get_frequency(void) {
    return timer_frequency_hz;
}