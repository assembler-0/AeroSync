/* SPDX-License-Identifier: GPL-2.0-only */

#include <aerosync/classes.h>
#include <aerosync/export.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/panic.h>
#include <aerosync/percpu.h>
#include <aerosync/sched/cpumask.h>
#include <aerosync/spinlock.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <lib/bitmap.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>

#ifndef CONFIG_PER_CPU_CHUNK_SIZE
#define CONFIG_PER_CPU_CHUNK_SIZE 64
#endif

#define PCPU_CHUNK_SIZE (CONFIG_PER_CPU_CHUNK_SIZE * 1024)

unsigned long __per_cpu_offset[MAX_CPUS];
static bool g_percpu_ready = false;

/* Dynamic per-cpu allocation state */
static DEFINE_SPINLOCK(pcpu_lock);
static unsigned long *pcpu_bitmap;
static uint16_t *pcpu_unit_counts; /* Track number of units per allocation */
static int pcpu_nr_bits;
static unsigned long pcpu_start_offset; /* Offset where dynamic area starts */

int percpu_ready(void) { return g_percpu_ready; }
EXPORT_SYMBOL(percpu_ready);

void setup_per_cpu_areas(void) {
  unsigned long static_size;
  unsigned long i;
  unsigned long count;
  void *ptr;

  static_size = (unsigned long)_percpu_end - (unsigned long)_percpu_start;

  if (PCPU_CHUNK_SIZE < static_size) {
    panic("PCPU_CHUNK_SIZE (%d) is smaller than static percpu section (%ld)!\n",
          PCPU_CHUNK_SIZE, static_size);
  }

  if (smp_get_cpu_count() == 0) {
    smp_parse_topology();
  }

  count = smp_get_cpu_count();
  if (count == 0)
    count = 1;

  printk(KERN_INFO PERCPU_CLASS "Setting up per-cpu data for %lu CPUs, chunk "
                                "size: %d bytes (static: %lu)\n",
         count, PCPU_CHUNK_SIZE, static_size);

  size_t pages = (PCPU_CHUNK_SIZE / PAGE_SIZE);
  if (PCPU_CHUNK_SIZE % PAGE_SIZE != 0)
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

    memset(ptr, 0, PCPU_CHUNK_SIZE);
    memcpy(ptr, _percpu_start, static_size);

    __per_cpu_offset[i] = (unsigned long)ptr - (unsigned long)_percpu_start;

    /* Set 'this_cpu_off' inside the per-cpu area */
    DECLARE_PER_CPU(unsigned long, this_cpu_off);
    *(unsigned long *)((uint8_t *)ptr + ((unsigned long)&this_cpu_off -
                                         (unsigned long)_percpu_start)) =
        __per_cpu_offset[i];

    printk(KERN_DEBUG PERCPU_CLASS "  CPU %lu: per-cpu area @ %p\n", i, ptr);
  }

  for (; i < MAX_CPUS; i++) {
    __per_cpu_offset[i] = 0;
  }

  /* Initialize dynamic allocator */
  pcpu_start_offset = ALIGN(static_size, 16);
  pcpu_nr_bits = (PCPU_CHUNK_SIZE - pcpu_start_offset) / 16;
  pcpu_bitmap = kzalloc(BITMAP_SIZE(pcpu_nr_bits));
  if (!pcpu_bitmap) {
    panic("Failed to allocate per-cpu bitmap");
  }

  pcpu_unit_counts = kzalloc(sizeof(uint16_t) * pcpu_nr_bits);
  if (!pcpu_unit_counts) {
    panic("Failed to allocate per-cpu unit counts array");
  }

  wrmsr(MSR_GS_BASE, __per_cpu_offset[0]);
  g_percpu_ready = true;

  printk(KERN_INFO PERCPU_CLASS
         "Full per-cpu setup done. Dynamic area: %d bytes\n",
         PCPU_CHUNK_SIZE - (int)pcpu_start_offset);
}

void *pcpu_alloc(size_t size, size_t align) {
  if (size == 0)
    return nullptr;

  /* We allocate in units of 16 bytes */
  int nr_units = (size + 15) / 16;
  int align_units = (align + 15) / 16;
  if (align_units == 0)
    align_units = 1;

  spinlock_lock(&pcpu_lock);

  int bit = (int)bitmap_find_next_zero_area(pcpu_bitmap, pcpu_nr_bits, 0,
                                            nr_units, align_units - 1);
  if (bit < 0 || bit >= pcpu_nr_bits) {
    spinlock_unlock(&pcpu_lock);
    return nullptr;
  }

  bitmap_set(pcpu_bitmap, bit, nr_units);
  pcpu_unit_counts[bit] = (uint16_t)nr_units;
  spinlock_unlock(&pcpu_lock);

  unsigned long offset = pcpu_start_offset + (bit * 16);
  return (void *)((unsigned long)_percpu_start + offset);
}

void pcpu_free(void *ptr) {
  if (!ptr)
    return;

  unsigned long offset = (unsigned long)ptr - (unsigned long)_percpu_start;
  if (offset < pcpu_start_offset || offset >= PCPU_CHUNK_SIZE) {
    return;
  }

  int bit = (offset - pcpu_start_offset) / 16;

  spinlock_lock(&pcpu_lock);
  if (!test_bit(bit, pcpu_bitmap)) {
    spinlock_unlock(&pcpu_lock);
    printk(KERN_ERR PERCPU_CLASS
           "Double free or invalid free of per-cpu pointer %p\n",
           ptr);
    return;
  }

  int nr_units = pcpu_unit_counts[bit];
  bitmap_clear(pcpu_bitmap, bit, nr_units);
  pcpu_unit_counts[bit] = 0;
  spinlock_unlock(&pcpu_lock);
}

EXPORT_SYMBOL(pcpu_alloc);
EXPORT_SYMBOL(pcpu_free);

void percpu_test(void) {
  int __percpu *p;
  int cpu;

  printk(KERN_INFO TEST_CLASS "Starting Per-CPU dynamic allocation test...\n");

  p = alloc_percpu(int);
  if (!p) {
    printk(KERN_ERR TEST_CLASS "Failed to allocate per-cpu int\n");
    return;
  }

  /* Initialize each CPU's copy */
  for_each_possible_cpu(cpu) { *per_cpu_ptr(*p, cpu) = cpu + 100; }

  /* Verify each CPU's copy */
  for_each_possible_cpu(cpu) {
    if (*per_cpu_ptr(*p, cpu) != cpu + 100) {
      printk(KERN_ERR TEST_CLASS
             "Per-CPU verification failed for CPU %d (got %d, expected %d)\n",
             cpu, *per_cpu_ptr(*p, cpu), cpu + 100);
      goto out;
    }
  }

  printk(KERN_INFO TEST_CLASS "Per-CPU dynamic allocation test passed!\n");

out:
  free_percpu(p);
}
