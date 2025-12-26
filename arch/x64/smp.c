/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/smp.c
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

#include <arch/x64/cpu.h>
#include <arch/x64/features/features.h>
#include <arch/x64/gdt/gdt.h>
#include <arch/x64/idt/idt.h>
#include <arch/x64/mm/vmm.h>
#include <arch/x64/smp.h>
#include <kernel/sysintf/ic.h>
#include <kernel/classes.h>
#include <kernel/wait.h>
#include <lib/printk.h>
#include <limine/limine.h>

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

// Global array to map logical CPU ID to physical APIC ID
int per_cpu_apic_id[MAX_CPUS];

// The entry point for Application Processors (APs)
static void smp_ap_entry(struct limine_mp_info *info) {
  // Switch to kernel page table
  vmm_switch_pml4(g_kernel_pml4);

  // Initialize APIC for this AP IMMEDIATELY so we can get our CPU ID
  // and use per-CPU caches in kmalloc()
  ic_ap_init();

  // Enable per-CPU features (SSE, AVX, etc.)
  cpu_features_init_ap();

  // Basic per-CPU init for APs
  printk(KERN_DEBUG SMP_CLASS "CPU LAPIC ID %u starting up...\n", info->lapic_id);

  ic_set_timer(IC_DEFAULT_TICK);

  // Initialize GDT and TSS for this AP
  gdt_init_ap();

  // Load IDT for this CPU
  idt_load(&g_IdtPtr);

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

void smp_init(void) {
  struct limine_mp_response *mp_response = mp_request.response;

  if (!mp_response) {
    printk(KERN_WARNING SMP_CLASS "Limine MP response not found. Single core mode.\n");
    cpu_count = 1;
    return;
  }

  cpu_count = mp_response->cpu_count;
  uint64_t bsp_lapic_id = mp_response->bsp_lapic_id;

  printk(KERN_DEBUG SMP_CLASS "Detected %llu CPUs. BSP LAPIC ID: %u\n", cpu_count,
         (uint32_t)bsp_lapic_id);

  // Initialize the wait counter for AP startup
  int expected_aps = (int)(cpu_count > 0 ? (cpu_count - 1) : 0);
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
    per_cpu_apic_id[i] = cpu->lapic_id;
  }

  // Ensure per_cpu_apic_id is visible to all CPUs before waking them
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // Set smp_initialized = 1 NOW, so APs can use their own caches from the start!
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

uint64_t smp_get_cpu_count(void) { return cpu_count; }

int smp_is_active() { return smp_initialized; }

uint64_t smp_get_id(void) {
  // Use Local APIC ID
  uint8_t lapic_id = ic_lapic_get_id();

  // Find logical ID from physical APIC ID
  for (uint64_t i = 0; i < cpu_count; i++) {
    if (per_cpu_apic_id[i] == (int)lapic_id) {
      return i;
    }
  }

  return 0; // Fallback to 0
}
