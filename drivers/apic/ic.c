#include <drivers/apic/apic.h>
#include <kernel/classes.h>
#include <kernel/types.h>
#include <lib/printk.h>
#include <drivers/apic/ic.h>
#include <kernel/panic.h>

#define MAX_CONTROLLERS 8

static interrupt_controller_interface_t *current_controller = NULL;
static const interrupt_controller_interface_t *registered_controllers[MAX_CONTROLLERS];
static size_t num_registered_controllers = 0;
static uint32_t timer_frequency_hz = 100; // default - can be changed at runtime

void ic_register_controller(const interrupt_controller_interface_t* controller) {
    if (num_registered_controllers >= MAX_CONTROLLERS) {
        printk(KERN_WARNING IC_CLASS "Max interrupt controllers registered, ignoring.\n");
        return;
    }
    registered_controllers[num_registered_controllers++] = controller;
}

interrupt_controller_t ic_install(void) {
    const interrupt_controller_interface_t* selected = NULL;
    const interrupt_controller_interface_t* fallback = NULL;

    for (size_t i = 0; i < num_registered_controllers; i++) {
        if (registered_controllers[i]->probe()) {
            if (!selected || registered_controllers[i]->priority > selected->priority) {
                fallback = selected;
                selected = registered_controllers[i];
            } else if (!fallback || registered_controllers[i]->priority > fallback->priority) {
                fallback = registered_controllers[i];
            }
        }
    }

    // Try selected, fall back if install fails
    if (selected && !selected->install()) {
        printk(KERN_WARNING IC_CLASS "Controller type %d install failed, trying fallback...\n", selected->type);
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
    current_controller = (interrupt_controller_interface_t*)selected;
    
    if (current_controller->type == INTC_APIC) {
        printk(KERN_INFO APIC_CLASS "APIC initialized successfully\n");
    } else {
        printk(KERN_INFO PIC_CLASS "PIC initialized successfully\n");
    }
    return current_controller->type;
}

void ic_shutdown_controller(void) {
    if (!current_controller) return;
    
    printk(KERN_INFO IC_CLASS "Shutting down interrupt controller...\n");
    
    // Mask all interrupts first to ensure silence
    if (current_controller->mask_all) {
        current_controller->mask_all();
    }
    
    // Perform specific shutdown logic if available
    if (current_controller->shutdown) {
        current_controller->shutdown();
    }
    
    current_controller = NULL;
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