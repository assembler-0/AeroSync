///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/san/scrub.c
 * @brief Background MM Consistency Scrubber
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sched/sched.h>
#include <aerosync/panic.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <mm/zone.h>
#include <mm/vma.h>
#include <aerosync/sched/process.h>
#include <aerosync/sysintf/time.h>
#include <aerosync/errno.h>

#define SCRUB_INTERVAL_MS 5000

static void scrub_pmm(void) {
  pmm_verify();
}

static void scrub_slab(void) {
  slab_verify_all();
}

static int mm_scrubberd(void *data) {
  (void) data;
  printk(KERN_INFO VMM_CLASS "mm_scrubberd started\n");

  while (1) {
    delay_ms(SCRUB_INTERVAL_MS);

    scrub_pmm();
    scrub_slab();

    /* Verify init_mm VMAs */
    down_read(&init_mm.mmap_lock);
    if (vma_verify_tree(&init_mm) != 0) {
      panic("mm_scrubber: init_mm VMA tree corruption detected!\n");
    }
    up_read(&init_mm.mmap_lock);
  }
  return 0;
}

int mm_scrubber_init(void) {
  struct task_struct *t = kthread_create(mm_scrubberd, nullptr, "mm_scrubberd");
  if (!t)
    return -ENOMEM;
  kthread_run(t);
  return 0;
}
