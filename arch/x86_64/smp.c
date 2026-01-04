/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/smp.c
 * @brief SMP initialization and AP entry point
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/entry.h>
#include <arch/x86_64/features/features.h>
#include <arch/x86_64/gdt/gdt.h>
#include <arch/x86_64/idt/idt.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/classes.h>
#include <kernel/sysintf/ic.h>
#include <kernel/wait.h>
#include <kernel/sysintf/panic.h>
#include <lib/printk.h>
#include <limine/limine.h>
#include <mm/slab.h>
#include <linux/container_of.h>
// SMP Request
__attribute__((
  used, section(".limine_requests"))) static volatile struct limine_mp_request
mp_request = {.id = LIMINE_MP_REQUEST_ID, .revision = 0};

static uint64_t cpu_count = 0;
static volatile int cpus_online = 0;
volatile int smp_lock = 0;
static volatile int smp_start_barrier =
    0; // BSP releases APs to start interrupts
static struct wait_counter ap_startup_counter;
static int smp_initialized = 0;

uint8_t lapic_get_id_for_cpu(int cpu);

// Per-CPU APIC ID
DEFINE_PER_CPU(int, cpu_apic_id);

DEFINE_PER_CPU(int, cpu_number);

// Per-CPU Call Queue
DEFINE_PER_CPU(struct smp_call_queue, cpu_call_queue);

void smp_init_cpu(int cpu) {
  struct smp_call_queue *q = per_cpu_ptr(cpu_call_queue, cpu);
  INIT_LIST_HEAD(&q->list);
  spinlock_init(&q->lock);
}

// The entry point for Application Processors (APs)
static void smp_ap_entry(struct limine_mp_info *info) {
  // Switch to kernel page table
  vmm_switch_pml_root(g_kernel_pml_root);

  // Find our logical ID (we can't use smp_get_id yet as GS base is not set)
  int cpu_id = 0;
  for (int i = 0; i < smp_get_cpu_count(); i++) {
    if (*per_cpu_ptr(cpu_apic_id, i) == (int) info->lapic_id) {
      cpu_id = i;
      break;
    }
  }

  // Set GS Base to point to per-cpu area
  wrmsr(MSR_GS_BASE, __per_cpu_offset[cpu_id]);
  // Initialize cpu_number for this CPU
  this_cpu_write(cpu_number, cpu_id);

  // Initialize per-cpu PMM cache
  pmm_init_cpu();

  // Initialize call queue
  smp_init_cpu(cpu_id);

  // Initialize APIC for this AP IMMEDIATELY so we can get our CPU ID
  // and use per-CPU caches in kmalloc()
  ic_ap_init();

  // ... rest of function ...

  // Enable per-CPU features (SSE, AVX, etc.)
  cpu_features_init_ap();

  // Basic per-CPU init for APs
  printk(KERN_DEBUG SMP_CLASS "CPU LAPIC ID %u starting up...\n",
         info->lapic_id);

  ic_set_timer(IC_DEFAULT_TICK);

  // Initialize GDT and TSS for this AP
  gdt_init_ap();

  // Load IDT for this CPU
  idt_load(&g_IdtPtr);

  // Initialize Syscall MSRs
  syscall_init();

  // Also increment the atomic counter for consistency with other code
  __atomic_fetch_add(&cpus_online, 1, __ATOMIC_RELEASE);

  // Mark this AP as online using wait counter
  wait_counter_inc(&ap_startup_counter);

  // Wait until BSP releases start barrier before enabling interrupts
  while (!__atomic_load_n(&smp_start_barrier, __ATOMIC_ACQUIRE)) {
    cpu_relax();
  }

  printk(KERN_DEBUG SMP_CLASS "CPU LAPIC ID %u online.\n", info->lapic_id);

  // Initialize scheduler for this AP
  sched_init_ap();

  // Enable interrupts for this CPU
  cpu_sti();

  // Scheduler Loop
  while (1) {
    check_preempt();
    cpu_hlt();
  }
}

void smp_parse_topology(void) {
  struct limine_mp_response *mp_response = mp_request.response;

  if (!mp_response) {
    printk(KERN_WARNING SMP_CLASS
      "Limine MP response not found. Single core mode.\n");
    cpu_count = 1;
    return;
  }

  cpu_count = mp_response->cpu_count;
}

void smp_call_ipi_handler(void) {
  struct smp_call_queue *q = this_cpu_ptr(cpu_call_queue);
  struct list_head local_list;
  INIT_LIST_HEAD(&local_list);

  // Swap list content under lock to minimize contention
  irq_flags_t flags = spinlock_lock_irqsave(&q->lock);
  list_splice_init(&q->list, &local_list);
  spinlock_unlock_irqrestore(&q->lock, flags);

  struct call_single_data *csd, *tmp;
  list_for_each_entry_safe(csd, tmp, &local_list, list) {
    csd->func(csd->info);
    if (csd->flags & CSD_FLAG_WAIT) {
      __atomic_store_n(&csd->flags, csd->flags & ~CSD_FLAG_WAIT, __ATOMIC_RELEASE);
    } else {
      // If it wasn't waiting, it must have been dynamically allocated.
      // For now, we only support stack-based waiting calls or fixed allocations.
      // kfree(csd); // Not implemented yet
    }
  }
}

void smp_call_function_single(int cpu, smp_call_func_t func, void *info, bool wait) {
  if (cpu == (int) smp_get_id()) {
    func(info);
    return;
  }

  struct call_single_data data;
  struct call_single_data *csd = &data;

  if (!wait) {
    // FIXME: Handle async calls by allocating CSD dynamically.
    // For now, we only support sync calls safely.
    panic("smp_call_function_single: async calls not yet supported\n");
  }

  csd->func = func;
  csd->info = info;
  csd->flags = CSD_FLAG_WAIT;
  INIT_LIST_HEAD(&csd->list);

  struct smp_call_queue *q = per_cpu_ptr(cpu_call_queue, cpu);

  irq_flags_t flags = spinlock_lock_irqsave(&q->lock);
  list_add_tail(&csd->list, &q->list);
  spinlock_unlock_irqrestore(&q->lock, flags);

  uint8_t lapic_id = (uint8_t) *per_cpu_ptr(cpu_apic_id, cpu);
  ic_send_ipi(lapic_id, CALL_FUNCTION_IPI_VECTOR, 0);

  if (wait) {
    while (__atomic_load_n(&csd->flags, __ATOMIC_ACQUIRE) & CSD_FLAG_WAIT) {
      cpu_relax();
    }
  }
}

void smp_call_function_many(const struct cpumask *mask, smp_call_func_t func, void *info, bool wait) {
  if (!smp_is_active()) {
    func(info);
    return;
  }

  int this_cpu = (int) smp_get_id();

  // For many, we use an array of CSDs if waiting
  if (wait) {
    struct call_single_data *csds = kmalloc(sizeof(struct call_single_data) * MAX_CPUS);
    int target_count = 0;
    int targets[MAX_CPUS];

    for (int i = 0; i < (int) smp_get_cpu_count(); i++) {
      if (i == this_cpu) continue;
      if (cpumask_test_cpu(i, mask)) {
        struct call_single_data *csd = &csds[i];
        csd->func = func;
        csd->info = info;
        csd->flags = CSD_FLAG_WAIT;
        INIT_LIST_HEAD(&csd->list);

        struct smp_call_queue *q = per_cpu_ptr(cpu_call_queue, i);
        irq_flags_t f = spinlock_lock_irqsave(&q->lock); // wait, fix this
        list_add_tail(&csd->list, &q->list);
        spinlock_unlock_irqrestore(&q->lock, f);

        targets[target_count++] = i;
        ic_send_ipi(lapic_get_id_for_cpu(i), CALL_FUNCTION_IPI_VECTOR, 0);
      }
    }

    for (int i = 0; i < target_count; i++) {
      struct call_single_data *csd = &csds[targets[i]];
      while (__atomic_load_n(&csd->flags, __ATOMIC_ACQUIRE) & CSD_FLAG_WAIT) {
        cpu_relax();
      }
    }
    kfree(csds);
  } else {
    panic("smp_call_function_many: async not supported\n");
  }
}

void smp_call_function(smp_call_func_t func, void *info, bool wait) {
  struct cpumask all;
  cpumask_setall(&all);
  smp_call_function_many(&all, func, info, wait);
}


void smp_init(void) {
  if (cpu_count == 0) {
    smp_parse_topology();
  }

  struct limine_mp_response *mp_response = mp_request.response;

  if (cpu_count == 1) {
    return;
  }
  uint64_t bsp_lapic_id = mp_response->bsp_lapic_id;

  printk(KERN_DEBUG SMP_CLASS "Detected %llu CPUs. BSP LAPIC ID: %u\n",
         cpu_count, (uint32_t) bsp_lapic_id);

  // Initialize the wait counter for AP startup
  int expected_aps = (int) (cpu_count > 0 ? (cpu_count - 1) : 0);
  init_wait_counter(&ap_startup_counter, 0, expected_aps);

  // Initialize per_cpu_apic_id array FIRST, before waking any APs
  uint64_t max_init = cpu_count < MAX_CPUS ? cpu_count : MAX_CPUS;
  if (cpu_count > MAX_CPUS) {
    printk(KERN_WARNING SMP_CLASS
           "Warning: CPU count %llu exceeds MAX_CPUS %d, limiting to %d\n",
           cpu_count, MAX_CPUS, MAX_CPUS);
  }
  for (uint64_t i = 0; i < max_init; i++) {
    struct limine_mp_info *cpu = mp_response->cpus[i];
    *per_cpu_ptr(cpu_apic_id, i) = cpu->lapic_id;
  }

  // Ensure per_cpu_apic_id is visible to all CPUs before waking them
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // Set smp_initialized = 1 NOW, so APs can use their own caches from the
  // start!
  smp_initialized = 1;

  // NOW iterate over CPUs and wake them up
  for (uint64_t i = 0; i < cpu_count; i++) {
    struct limine_mp_info *cpu = mp_response->cpus[i];

    if (cpu->lapic_id == bsp_lapic_id) {
      continue;
    }

    printk(KERN_DEBUG SMP_CLASS "Waking up CPU LAPIC ID: %u\n", cpu->lapic_id);
    __atomic_store_n(&cpu->goto_address, smp_ap_entry, __ATOMIC_RELEASE);
  }

  wait_counter_wait(&ap_startup_counter);
  __atomic_store_n(&smp_start_barrier, 1, __ATOMIC_RELEASE);

  printk(SMP_CLASS "%d APs online.\n", cpus_online);
}

void smp_prepare_boot_cpu(void) {
  // BSP is always CPU 0
  this_cpu_write(cpu_number, 0);
}

uint64_t smp_get_cpu_count(void) { return cpu_count; }

int smp_is_active() { return smp_initialized; }

uint64_t smp_get_id(void) {
  // If GS is set up, this is fast O(1)
  // We assume GS is set up early enough (in setup_per_cpu_areas for BSP)
  return this_cpu_read(cpu_number);
}

int lapic_to_cpu(uint8_t lapic_id) {
  for (int i = 0; i < smp_get_cpu_count(); i++) {
    if (*per_cpu_ptr(cpu_apic_id, i) == (int) lapic_id) {
      return i;
    }
  }
  return -1;
}

uint8_t lapic_get_id_for_cpu(int cpu) {
  if (cpu < 0 || cpu >= MAX_CPUS) return 0xFF;
  return (uint8_t) *per_cpu_ptr(cpu_apic_id, cpu);
}
