#include <kernel/classes.h>
#include <arch/x64/smp.h>
#include <limine/limine.h>
#include <lib/printk.h>
#include <arch/x64/cpu.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <drivers/apic/apic.h>

// SMP Request
__attribute__((used, section(".limine_requests"))) 
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0
};

static uint64_t cpu_count = 0;
static volatile int cpus_online = 0;
volatile int smp_lock = 0;
static volatile int smp_start_barrier = 0; // BSP releases APs to start interrupts

// Global array to map logical CPU ID to physical APIC ID
// MAX_CPUS should be defined elsewhere (e.g., in sched.h) and needs to be visible here.
// For now, hardcode MAX_CPUS.
#define MAX_CPUS 32 // TODO: Get from sched.h or a global config
int per_cpu_apic_id[MAX_CPUS];

// The entry point for Application Processors (APs)
static void smp_ap_entry(struct limine_mp_info *info) {
    // Basic per-CPU init for APs
    printk(SMP_CLASS "CPU LAPIC ID %u starting up...\n", info->lapic_id);

    // Load IDT for this CPU
    idt_install();

    // Mark this AP as online
    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_RELEASE);

    // Wait until BSP releases start barrier before enabling interrupts
    while (!__atomic_load_n(&smp_start_barrier, __ATOMIC_ACQUIRE)) {
        cpu_relax();
    }

    printk(SMP_CLASS "CPU LAPIC ID %u online.\n", info->lapic_id);

    // Idle loop for now
    while (1) {
        cpu_hlt();
    }
}

void smp_init(void) {
    struct limine_mp_response *mp_response = mp_request.response;

    if (!mp_response) {
        printk(SMP_CLASS "Limine MP response not found. Single core mode.\n");
        cpu_count = 1;
        return;
    }

    cpu_count = mp_response->cpu_count;
    uint64_t bsp_lapic_id = mp_response->bsp_lapic_id;

    printk(SMP_CLASS "Detected %llu CPUs. BSP LAPIC ID: %u\n", cpu_count, (uint32_t)bsp_lapic_id);

    // Initialize per_cpu_apic_id array
    for (uint64_t i = 0; i < cpu_count; i++) {
        struct limine_mp_info *cpu = mp_response->cpus[i];
        per_cpu_apic_id[i] = cpu->lapic_id;
    }

    // Iterate over CPUs and wake them up
    for (uint64_t i = 0; i < cpu_count; i++) {
        struct limine_mp_info *cpu = mp_response->cpus[i];
        
        if (cpu->lapic_id == bsp_lapic_id) {
            // This is the BSP (us)
            continue;
        }

        // Send the AP to our entry point
        // Limine handles the trampoline for us!
        printk(SMP_CLASS "Waking up CPU LAPIC ID: %u\n", cpu->lapic_id);
        __atomic_store_n(&cpu->goto_address, smp_ap_entry, __ATOMIC_RELEASE);
    }

    // Wait until all APs have reported online
    int expected_aps = (int)(cpu_count > 0 ? (cpu_count - 1) : 0);
    while (__atomic_load_n(&cpus_online, __ATOMIC_ACQUIRE) < expected_aps) {
        cpu_relax();
    }

    // Release APs to enable interrupts and proceed
    __atomic_store_n(&smp_start_barrier, 1, __ATOMIC_RELEASE);

    printk(SMP_CLASS "%d APs online.\n", cpus_online);
}

uint64_t smp_get_cpu_count(void) {
    return cpu_count;
}

uint64_t smp_get_id(void) {
    // Use Local APIC ID when available
    return (uint64_t)lapic_get_id();
}
