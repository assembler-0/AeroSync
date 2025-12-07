#include <kernel/classes.h>
#include <arch/x64/smp.h>
#include <limine/limine.h>
#include <lib/printk.h>
#include <arch/x64/cpu.h>

// SMP Request
__attribute__((used, section(".limine_requests"))) 
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0
};

static uint64_t cpu_count = 0;
static volatile int cpus_online = 0;
volatile int smp_lock = 0;

// The entry point for Application Processors (APs)
static void smp_ap_entry(struct limine_mp_info *info) {

    // 2. TODO: Load IDT
    // 3. TODO: Enable LAPIC
    
    // Increment online count atomically
    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_RELEASE);

    // Signal that we are online (debug)
    // Note: printk is not strictly thread-safe without locks, be careful.
    // We'll skip printk here to avoid race conditions on the serial port/buffer for now.

    // Loop forever
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

    // Wait a bit for APs to come online (simple spin)
    // In a real kernel, you'd sync more robustly
    uint64_t start_wait = 0; // rdtsc(); // If we had a calibrated timer...
    // Just a simple loop for demo
    for(volatile int k=0; k<10000000; k++);

    printk(SMP_CLASS "%d APs online.\n", cpus_online);
}

uint64_t smp_get_cpu_count(void) {
    return cpu_count;
}

uint64_t smp_get_id(void) {
    // This is tricky without per-cpu data set up.
    // We would need to read the Local APIC ID register.
    // For now, return 0 (BSP) as a placeholder or implement LAPIC read.
    return 0; 
}
