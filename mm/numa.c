/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/numa.c
 * @brief Early NUMA topology discovery using raw ACPI parsing
 * @copyright (C) 2025 assembler-0
 */

#include <mm/zone.h>
#include <mm/page.h>
#include <lib/printk.h>
#include <uacpi/acpi.h> // We only use the header definitions
#include <kernel/classes.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/string.h>

struct pglist_data *node_data[MAX_NUMNODES] = {NULL};
static struct pglist_data static_node_data[MAX_NUMNODES];
struct numa_mem_range {
    uint64_t start_pfn;
    uint64_t end_pfn;
    int nid;
};

#define MAX_NUMA_RANGES 32
static struct numa_mem_range numa_ranges[MAX_NUMA_RANGES];
static int numa_range_count = 0;

/* Physical LAPIC ID to Node mapping */
static struct {
    uint8_t lapic_id;
    int nid;
} lapic_node_map[MAX_CPUS];
static int lapic_node_count = 0;

int cpu_to_node(int cpu) {
    extern uint8_t lapic_get_id_for_cpu(int cpu);
    uint8_t lapic_id = lapic_get_id_for_cpu(cpu);
    
    for (int i = 0; i < lapic_node_count; i++) {
        if (lapic_node_map[i].lapic_id == lapic_id)
            return lapic_node_map[i].nid;
    }
    return 0;
}

int pfn_to_nid(uint64_t pfn) {
    for (int i = 0; i < numa_range_count; i++) {
        if (pfn >= numa_ranges[i].start_pfn && pfn < numa_ranges[i].end_pfn) {
            return numa_ranges[i].nid;
        }
    }
    return 0;
}

static void parse_srat(struct acpi_srat *srat) {
    uint8_t *ptr = (uint8_t *)(srat + 1);
    uint8_t *end = (uint8_t *)srat + srat->hdr.length;

    while (ptr < end) {
        struct acpi_entry_hdr *ehdr = (struct acpi_entry_hdr *)ptr;
        if (ehdr->length == 0) break;

        switch (ehdr->type) {
            case ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY: {
                struct acpi_srat_processor_affinity *la = (void*)ehdr;
                if (!(la->flags & ACPI_SRAT_PROCESSOR_ENABLED)) break;
                
                uint32_t domain = la->proximity_domain_low;
                domain |= (uint32_t)la->proximity_domain_high[0] << 8;
                domain |= (uint32_t)la->proximity_domain_high[1] << 16;
                domain |= (uint32_t)la->proximity_domain_high[2] << 24;

                if (lapic_node_count < MAX_CPUS) {
                    lapic_node_map[lapic_node_count].lapic_id = la->id;
                    lapic_node_map[lapic_node_count].nid = (int)domain;
                    lapic_node_count++;
                }
                break;
            }
            case ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY: {
                struct acpi_srat_memory_affinity *ma = (void*)ehdr;
                if (!(ma->flags & ACPI_SRAT_MEMORY_ENABLED)) break;
                
                uint32_t domain = ma->proximity_domain;
                if (domain < MAX_NUMNODES) {
                    if (node_data[domain] == NULL) {
                        node_data[domain] = &static_node_data[domain];
                        node_data[domain]->node_id = domain;
                        node_data[domain]->node_start_pfn = ~0UL;
                        node_data[domain]->node_spanned_pages = 0;
                    }

                    uint64_t start_pfn = ma->address >> 12;
                    uint64_t end_pfn = (ma->address + ma->length) >> 12;

                    if (start_pfn < node_data[domain]->node_start_pfn)
                        node_data[domain]->node_start_pfn = start_pfn;
                    
                    uint64_t total_end = start_pfn + (ma->length >> 12);
                    if (total_end > (node_data[domain]->node_start_pfn + node_data[domain]->node_spanned_pages))
                        node_data[domain]->node_spanned_pages = total_end - node_data[domain]->node_start_pfn;

                    if (numa_range_count < MAX_NUMA_RANGES) {
                        numa_ranges[numa_range_count].start_pfn = start_pfn;
                        numa_ranges[numa_range_count].end_pfn = end_pfn;
                        numa_ranges[numa_range_count].nid = (int)domain;
                        numa_range_count++;
                        
                        printk(KERN_DEBUG "[NUMA] Range [%llx - %llx] -> Node %d\n", 
                               ma->address, ma->address + ma->length, domain);
                    }
                }
                break;
            }
        }
        ptr += ehdr->length;
    }
}

void numa_init(void *rsdp_ptr) {
    if (!rsdp_ptr) {
        printk(KERN_INFO NUMA_CLASS "No RSDP provided, assuming UMA.\n");
        goto fallback;
    }

    struct acpi_rsdp *rsdp = (struct acpi_rsdp *)rsdp_ptr;
    struct acpi_xsdt *xsdt = NULL;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        xsdt = (struct acpi_xsdt *)pmm_phys_to_virt(rsdp->xsdt_addr);
    } else {
        printk(KERN_WARNING NUMA_CLASS "No XSDT found (legacy ACPI), assuming UMA.\n");
        goto fallback;
    }

    size_t entry_count = (xsdt->hdr.length - sizeof(struct acpi_sdt_hdr)) / sizeof(uint64_t);
    for (size_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_hdr *table = (struct acpi_sdt_hdr *)pmm_phys_to_virt(xsdt->entries[i]);
        if (memcmp(table->signature, ACPI_SRAT_SIGNATURE, 4) == 0) {
            printk(KERN_DEBUG NUMA_CLASS "Found SRAT at %p\n", table);
            parse_srat((struct acpi_srat *)table);
            return;
        }
    }

fallback:
    node_data[0] = &static_node_data[0];
    node_data[0]->node_id = 0;
    node_data[0]->node_start_pfn = 0;
    
    // Use a large enough value to cover all usable memory in UMA mode.
    // pmm_init will later use pmm_max_pages if this is UMA.
    // Setting it to a safe upper bound or 0 is handled in pmm_init by checking if pfn_to_nid returns 0.
    node_data[0]->node_spanned_pages = 0xFFFFFFFF; // Effectively entire space
}