/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/numa.c
 * @brief Early NUMA topology discovery using raw ACPI parsing
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/zone.h>
#include <mm/page.h>
#include <lib/printk.h>
#include <uacpi/acpi.h> // We only use the header definitions
#include <aerosync/classes.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/string.h>

#include <aerosync/sched/cpumask.h>

struct pglist_data *node_data[MAX_NUMNODES] = {NULL};
static struct pglist_data static_node_data[MAX_NUMNODES];

/* Global NUMA state */
int numa_enabled = 0;
int nr_node_ids = 1;
static struct cpumask node_to_cpumask_map[MAX_NUMNODES];

struct numa_mem_range {
  uint64_t start_pfn;
  uint64_t end_pfn;
  int nid;
};

#define MAX_NUMA_RANGES 32
static struct numa_mem_range numa_ranges[MAX_NUMA_RANGES];
static int numa_range_count = 0;

/* NUMA distance matrix */
#define NUMA_NO_DISTANCE 255
static uint8_t numa_distance[MAX_NUMNODES][MAX_NUMNODES];
static bool numa_distance_valid = false;

/* Physical LAPIC ID to Node mapping */
static struct {
  uint8_t lapic_id;
  int nid;
} lapic_node_map[MAX_CPUS];

static int lapic_node_count = 0;

/* Helper to get CPU mask for a node */
const struct cpumask *cpumask_of_node(int node) {
  if (node < 0 || node >= MAX_NUMNODES)
    return &cpu_online_mask; /* Fallback */
  return &node_to_cpumask_map[node];
}

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

int numa_distance_get(int from, int to) {
  if (!numa_distance_valid || from >= MAX_NUMNODES || to >= MAX_NUMNODES)
    return NUMA_NO_DISTANCE;
  return numa_distance[from][to];
}

int numa_find_best_node(int preferred_node) {
  if (preferred_node >= 0 && preferred_node < MAX_NUMNODES && node_data[preferred_node])
    return preferred_node;

  /* Find closest node if distance matrix is available */
  if (numa_distance_valid && preferred_node >= 0 && preferred_node < MAX_NUMNODES) {
    int best_node = -1;
    int best_distance = NUMA_NO_DISTANCE;

    for (int i = 0; i < MAX_NUMNODES; i++) {
      if (!node_data[i]) continue;
      int dist = numa_distance[preferred_node][i];
      if (dist < best_distance) {
        best_distance = dist;
        best_node = i;
      }
    }
    if (best_node >= 0) return best_node;
  }

  /* Fallback to first available node */
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (node_data[i]) return i;
  }
  return -1;
}

static void parse_srat(struct acpi_srat *srat) {
  uint8_t *ptr = (uint8_t *) (srat + 1);
  uint8_t *end = (uint8_t *) srat + srat->hdr.length;
  
  numa_enabled = 1;
  int max_nid = 0;

  /* Reset default CPU masks */
  for (int i = 0; i < MAX_NUMNODES; i++) cpumask_clear(&node_to_cpumask_map[i]);

  while (ptr < end) {
    struct acpi_entry_hdr *ehdr = (struct acpi_entry_hdr *) ptr;
    if (ehdr->length == 0) break;

    switch (ehdr->type) {
      case ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY: {
        struct acpi_srat_processor_affinity *la = (void *) ehdr;
        if (!(la->flags & ACPI_SRAT_PROCESSOR_ENABLED)) break;

        uint32_t domain = la->proximity_domain_low;
        domain |= (uint32_t) la->proximity_domain_high[0] << 8;
        domain |= (uint32_t) la->proximity_domain_high[1] << 16;
        domain |= (uint32_t) la->proximity_domain_high[2] << 24;
        
        if (domain > (uint32_t)max_nid) max_nid = (int)domain;
        
        /* Map LAPIC to Node */
        if (lapic_node_count < MAX_CPUS) {
          lapic_node_map[lapic_node_count].lapic_id = la->id;
          lapic_node_map[lapic_node_count].nid = (int) domain;
          lapic_node_count++;
        }
        break;
      }
      case ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY: {
        struct acpi_srat_memory_affinity *ma = (void *) ehdr;
        if (!(ma->flags & ACPI_SRAT_MEMORY_ENABLED)) break;

        uint32_t domain = ma->proximity_domain;
        if (domain < MAX_NUMNODES) {
          if (node_data[domain] == NULL) {
            node_data[domain] = &static_node_data[domain];
            node_data[domain]->node_id = domain;
            node_data[domain]->node_start_pfn = ~0UL;
            node_data[domain]->node_spanned_pages = 0;
          }
          
          if ((int)domain > max_nid) max_nid = (int)domain;

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
            numa_ranges[numa_range_count].nid = (int) domain;
            numa_range_count++;

            printk(KERN_DEBUG NUMA_CLASS "Range [%llx - %llx] -> Node %d\n",
                   ma->address, ma->address + ma->length, domain);
          }
        }
        break;
      }
    }
    ptr += ehdr->length;
  }
  nr_node_ids = max_nid + 1;
}

static void parse_slit(struct acpi_slit *slit) {
  uint64_t count = slit->num_localities;
  if (count > MAX_NUMNODES) count = MAX_NUMNODES;

  for (uint64_t i = 0; i < count; i++) {
    for (uint64_t j = 0; j < count; j++) {
      numa_distance[i][j] = slit->matrix[i * slit->num_localities + j];
    }
  }
  numa_distance_valid = true;
  printk(KERN_DEBUG NUMA_CLASS "Parsed SLIT with %llu nodes\n", count);
}

void numa_init(void *rsdp_ptr) {
  if (!rsdp_ptr) {
    printk(KERN_INFO NUMA_CLASS "No RSDP provided, assuming UMA.\n");
    goto fallback;
  }

  struct acpi_rsdp *rsdp = (struct acpi_rsdp *) rsdp_ptr;
  struct acpi_xsdt *xsdt = NULL;

  if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
    xsdt = (struct acpi_xsdt *) pmm_phys_to_virt(rsdp->xsdt_addr);
  } else {
    printk(KERN_WARNING NUMA_CLASS "No XSDT found (legacy ACPI), assuming UMA.\n");
    goto fallback;
  }

  size_t entry_count = (xsdt->hdr.length - sizeof(struct acpi_sdt_hdr)) / sizeof(uint64_t);
  bool found_srat = false;

  for (size_t i = 0; i < entry_count; i++) {
    struct acpi_sdt_hdr *table = (struct acpi_sdt_hdr *) pmm_phys_to_virt(xsdt->entries[i]);
    if (memcmp(table->signature, ACPI_SRAT_SIGNATURE, 4) == 0) {
      printk(KERN_DEBUG NUMA_CLASS "Found SRAT at %p\n", table);
      parse_srat((struct acpi_srat *) table);
      found_srat = true;
    } else if (memcmp(table->signature, ACPI_SLIT_SIGNATURE, 4) == 0) {
      printk(KERN_DEBUG NUMA_CLASS "Found SLIT at %p\n", table);
      parse_slit((struct acpi_slit *) table);
    }
  }

  if (found_srat) return;

fallback:
  node_data[0] = &static_node_data[0];
  node_data[0]->node_id = 0;
  node_data[0]->node_start_pfn = 0;

  // Use a large enough value to cover all usable memory in UMA mode.
  // pmm_init will later use pmm_max_pages if this is UMA.
  // Setting it to a safe upper bound or 0 is handled in pmm_init by checking if pfn_to_nid returns 0.
  node_data[0]->node_spanned_pages = 0xFFFFFFFF; // Effectively entire space
}

/**
 * numa_mem_id - Get the memory-local NUMA node for the current CPU
 *
 * Returns the NUMA node that has the best memory bandwidth for the current CPU.
 * This may differ from cpu_to_node() on systems with non-uniform memory access.
 * For most systems, this is the same as the CPU's node.
 */
int numa_mem_id(void) {
  /* For now, return the CPU's node. In the future, this could be enhanced
   * to return the node with the best memory bandwidth based on HMAT tables */
  extern int this_node(void);
  return this_node();
}

