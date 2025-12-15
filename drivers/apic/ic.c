#include <drivers/apic/apic.h>
#include <kernel/classes.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <drivers/apic/ic.h>
#include <drivers/apic/pic.h>
#include <kernel/panic.h>

static interrupt_controller_interface_t *current_controller = NULL;
static uint32_t timer_frequency_hz = 100; // default - can be changed at runtime

static const interrupt_controller_interface_t apic_interface = {
    .type = INTC_APIC,
    .probe = apic_probe,
    .install = apic_init,
    .timer_set = apic_timer_init,
    .enable_irq = apic_enable_irq,
    .disable_irq = apic_disable_irq,
    .send_eoi = apic_send_eoi,
    .mask_all = apic_mask_all,
    .priority = 100,
};

static const interrupt_controller_interface_t pic_interface = {
    .type = INTC_PIC,
    .probe = pic_probe,
    .install = pic_install,
    .timer_set = pit_set_frequency,
    .enable_irq = pic_enable_irq,
    .disable_irq = pic_disable_irq,
    .send_eoi = pic_send_eoi,
    .mask_all = pic_mask_all,
    .priority = 50,
};

static const interrupt_controller_interface_t* const controllers[] = {
    &apic_interface,
    &pic_interface,
};

static const size_t ic_num_controllers =
    sizeof(controllers) / sizeof(controllers[0]);

interrupt_controller_t ic_install(void) {
    const interrupt_controller_interface_t* selected = NULL;
    const interrupt_controller_interface_t* fallback = NULL;

    for (size_t i = 0; i < ic_num_controllers; i++) {
        if (controllers[i]->probe()) {
            if (!selected || controllers[i]->priority > selected->priority) {
                fallback = selected;
                selected = controllers[i];
            } else if (!fallback || controllers[i]->priority > fallback->priority) {
                fallback = controllers[i];
            }
        }
    }

    // Try selected, fall back if install fails
    if (selected && !selected->install()) {
        printk(KERN_WARNING IC_CLASS "%s install failed, trying fallback...\n",
               selected->type == INTC_APIC ? "APIC" : "PIC");
        selected = fallback;
        if (selected && !selected->install()) {
           selected = NULL;
        }
    }

    if (!selected) {
        panic(IC_CLASS "No interrupt controller could be installed\n");
    }

    printk(KERN_INFO IC_CLASS "Configuring timer to %u Hz...\n", timer_frequency_hz);
    selected->timer_set(timer_frequency_hz);
    selected->mask_all();
    printk(KERN_INFO IC_CLASS "Timer configured.\n");

    // Set current controller type
    if (selected == &apic_interface) {
        current_controller = (interrupt_controller_interface_t*)selected;
        printk(KERN_INFO APIC_CLASS "APIC initialized successfully\n");
    } else {
        current_controller = (interrupt_controller_interface_t*)selected;
        printk(KERN_INFO PIC_CLASS "PIC initialized successfully\n");
    }
    return current_controller->type;
}

void ic_enable_irq(uint8_t irq_line) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    if (!current_controller->enable_irq) panic(IC_CLASS "enable_irq not supported");
    current_controller->enable_irq(irq_line);
}

void ic_disable_irq(uint8_t irq_line) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    if (!current_controller->disable_irq) panic(IC_CLASS "disable_irq not supported");
    current_controller->disable_irq(irq_line);
}

void ic_send_eoi(uint32_t interrupt_number) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    if (!current_controller->send_eoi) panic(IC_CLASS "send_eoi not supported");
    current_controller->send_eoi(interrupt_number);
}

interrupt_controller_t ic_get_controller_type(void) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    return current_controller->type;
}

const char* ic_get_controller_name(void) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    switch (current_controller->type) {
        case INTC_APIC:
            return "APIC";
        case INTC_PIC:
        default:
            return "PIC";
    }
}

void ic_set_timer(const uint32_t frequency_hz) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    if (!current_controller->timer_set) panic(IC_CLASS "timer_set not supported");
    current_controller->timer_set(frequency_hz);
    timer_frequency_hz = frequency_hz;
}

void ic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    if (current_controller->type != INTC_APIC) {
        panic(IC_CLASS "IPIs only supported on APIC controllers");
    }
    apic_send_ipi(dest_apic_id, vector, delivery_mode);
}

void ic_mask_all() {
    if (!current_controller) panic(IC_CLASS "IC not initialized");
    if (!current_controller->mask_all) panic(IC_CLASS "mask_all not supported");
    current_controller->mask_all();
}

uint32_t ic_get_frequency(void) {
    return timer_frequency_hz;
}