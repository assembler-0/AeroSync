/* SPDX-License-Identifier: GPL-2.0-only */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/classes.h>
#include <kernel/fkx/fkx.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

unsigned long __per_cpu_offset[MAX_CPUS];
static bool g_percpu_ready = false;

bool percpu_ready(void) { return g_percpu_ready; }
EXPORT_SYMBOL(percpu_ready);

void setup_per_cpu_areas(void) {
  unsigned long size;
  unsigned long i;
  unsigned long count;
  void *ptr;

  size = ALIGN((unsigned long)_percpu_end - (unsigned long)_percpu_start,
               PAGE_SIZE);

  if (smp_get_cpu_count() == 0) {
    smp_parse_topology();
  }

  count = smp_get_cpu_count();
  if (count == 0)
    count = 1;

  printk(KERN_INFO PERCPU_CLASS
         "Setting up per-cpu data for %lu CPUs, size: %lu bytes\n",
         count, size);

  size_t pages = (size / PAGE_SIZE);
  if (size % PAGE_SIZE != 0)
    pages++;

  for (i = 0; i < count; i++) {
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) {
      printk(KERN_ERR PERCPU_CLASS
             "Failed to allocate per-cpu area for CPU %lu\n",
             i);
      panic("Per-CPU allocation failed");
    }
    ptr = pmm_phys_to_virt(phys);

    memset(ptr, 0, size);
    memcpy(ptr, _percpu_start,
           (unsigned long)_percpu_end - (unsigned long)_percpu_start);

    // With regular linking, _percpu_start is a high address (e.g.,
    // 0xFFFFFFFF80xxxxxx). The variables are accessed via GS segment:
    // %gs:offset. We want the access at 'offset' (which is &var - 0 if linked
    // at 0, but now is &var). So if accesses are generated as: mov %gs:(&var),
    // %reg We need GS_BASE + &var = ptr + (&var - _percpu_start) GS_BASE = ptr
    // - _percpu_start.
    __per_cpu_offset[i] = (unsigned long)ptr - (unsigned long)_percpu_start;

    // Set 'this_cpu_off' inside the per-cpu area
    DECLARE_PER_CPU(unsigned long, this_cpu_off);
    *(unsigned long *)((uint8_t *)ptr + ((unsigned long)&this_cpu_off - (unsigned long)_percpu_start)) = __per_cpu_offset[i];

    printk(KERN_DEBUG PERCPU_CLASS "  CPU %lu: per-cpu area @ %p\n", i, ptr);
  }

  for (; i < MAX_CPUS; i++) {
    __per_cpu_offset[i] = 0;
  }

  wrmsr(MSR_GS_BASE, __per_cpu_offset[0]);
  g_percpu_ready = true;

  printk(KERN_INFO PERCPU_CLASS "Full per-cpu setup done. BSP GS_BASE set.\n");
}
