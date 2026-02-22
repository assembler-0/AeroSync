/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/numa.c
 * @brief Early NUMA topology discovery using raw ACPI parsing
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/zone.h>
#include <acpi.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/string.h>
#include <aerosync/sched/cpumask.h>

struct pglist_data *node_data[MAX_NUMNODES] = {nullptr};
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
  return -ENODEV;
}

static void parse_srat(ACPI_TABLE_SRAT *srat) {
  uint8_t *ptr = (uint8_t *) (srat + 1);
  uint8_t *end = (uint8_t *) srat + srat->Header.Length;
  
  numa_enabled = 1;
  int max_nid = 0;

  /* Reset default CPU masks */
  for (int i = 0; i < MAX_NUMNODES; i++) cpumask_clear(&node_to_cpumask_map[i]);

  while (ptr < end) {
    ACPI_SUBTABLE_HEADER *ehdr = (ACPI_SUBTABLE_HEADER *) ptr;
    if (ehdr->Length == 0) break;

    switch (ehdr->Type) {
      case ACPI_SRAT_TYPE_CPU_AFFINITY: {
        ACPI_SRAT_CPU_AFFINITY *la = (void *) ehdr;
        if (!(la->Flags & ACPI_SRAT_CPU_ENABLED)) break;

        uint32_t domain = la->ProximityDomainLo;
        domain |= (uint32_t) la->ProximityDomainHi[0] << 8;
        domain |= (uint32_t) la->ProximityDomainHi[1] << 16;
        domain |= (uint32_t) la->ProximityDomainHi[2] << 24;
        
        if (domain > (uint32_t)max_nid) max_nid = (int)domain;
        
        /* Map LAPIC to Node */
        if (lapic_node_count < MAX_CPUS) {
          lapic_node_map[lapic_node_count].lapic_id = la->ApicId;
          lapic_node_map[lapic_node_count].nid = (int) domain;
          lapic_node_count++;
        }
        break;
      }
      case ACPI_SRAT_TYPE_MEMORY_AFFINITY: {
        ACPI_SRAT_MEM_AFFINITY *ma = (void *) ehdr;
        if (!(ma->Flags & ACPI_SRAT_MEM_ENABLED)) break;

        uint32_t domain = ma->ProximityDomain;
        if (domain < MAX_NUMNODES) {
          if (node_data[domain] == nullptr) {
            node_data[domain] = &static_node_data[domain];
            node_data[domain]->node_id = domain;
            node_data[domain]->node_start_pfn = ~0UL;
            node_data[domain]->node_spanned_pages = 0;
          }
          
          if ((int)domain > max_nid) max_nid = (int)domain;

          uint64_t start_pfn = ma->BaseAddress >> 12;
          uint64_t end_pfn = (ma->BaseAddress + ma->Length) >> 12;

          if (start_pfn < node_data[domain]->node_start_pfn)
            node_data[domain]->node_start_pfn = start_pfn;

          uint64_t total_end = start_pfn + (ma->Length >> 12);
          if (total_end > (node_data[domain]->node_start_pfn + node_data[domain]->node_spanned_pages))
            node_data[domain]->node_spanned_pages = total_end - node_data[domain]->node_start_pfn;

          if (numa_range_count < MAX_NUMA_RANGES) {
            numa_ranges[numa_range_count].start_pfn = start_pfn;
            numa_ranges[numa_range_count].end_pfn = end_pfn;
            numa_ranges[numa_range_count].nid = (int) domain;
            numa_range_count++;

            printk(KERN_DEBUG NUMA_CLASS "Range [%llx - %llx] -> Node %d\n",
                   ma->BaseAddress, ma->BaseAddress + ma->Length, domain);
          }
        }
        break;
      }
    }
    ptr += ehdr->Length;
  }
  nr_node_ids = max_nid + 1;
}

static void parse_slit(ACPI_TABLE_SLIT *slit) {
  uint64_t count = slit->LocalityCount;
  if (count > MAX_NUMNODES) count = MAX_NUMNODES;

  for (uint64_t i = 0; i < count; i++) {
    for (uint64_t j = 0; j < count; j++) {
      numa_distance[i][j] = slit->Entry[i * slit->LocalityCount + j];
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

  ACPI_TABLE_RSDP *rsdp = (ACPI_TABLE_RSDP *) rsdp_ptr;
  ACPI_TABLE_XSDT *xsdt = nullptr;

  if (rsdp->Revision >= 2 && rsdp->XsdtPhysicalAddress) {
    xsdt = (ACPI_TABLE_XSDT *) pmm_phys_to_virt(rsdp->XsdtPhysicalAddress);
  } else {
    printk(KERN_WARNING NUMA_CLASS "No XSDT found (legacy ACPI), assuming UMA.\n");
    goto fallback;
  }

  size_t entry_count = (xsdt->Header.Length - sizeof(ACPI_TABLE_HEADER)) / sizeof(uint64_t);
  bool found_srat = false;

  for (size_t i = 0; i < entry_count; i++) {
    ACPI_TABLE_HEADER *table = (ACPI_TABLE_HEADER *) pmm_phys_to_virt(xsdt->TableOffsetEntry[i]);
    if (memcmp(table->Signature, ACPI_SIG_SRAT, 4) == 0) {
      printk(KERN_DEBUG NUMA_CLASS "Found SRAT at %p\n", table);
      parse_srat((ACPI_TABLE_SRAT *) table);
      found_srat = true;
    } else if (memcmp(table->Signature, ACPI_SIG_SLIT, 4) == 0) {
      printk(KERN_DEBUG NUMA_CLASS "Found SLIT at %p\n", table);
      parse_slit((ACPI_TABLE_SLIT *) table);
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