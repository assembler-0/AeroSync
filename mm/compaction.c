///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/compaction.c
 * @brief Memory compaction for fragmentation reduction (Defragmenter)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/mm_types.h>
#include <mm/zone.h>
#include <mm/page.h>
#include <mm/vm_object.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/list.h>
#include <aerosync/classes.h>
#include <aerosync/sched/process.h>
#include <aerosync/sysintf/time.h>
#include <mm/vma.h>

/**
 * struct compact_control - Control state for compaction.
 */
struct compact_control {
  struct zone *zone;
  unsigned long free_pfn; /* Scanner for free pages (upwards) */
  unsigned long migrate_pfn; /* Scanner for movable pages (downwards) */
  struct list_head migratepages;
  struct list_head freepages;
  size_t nr_migratepages;
  size_t nr_freepages;
  int order; /* Target order for compaction */
  gfp_t gfp_mask;
};

/**
 * isolate_migratepages - Scan zone for movable pages to migrate.
 */
static int isolate_migratepages(struct compact_control *cc) {
  unsigned long pfn = cc->migrate_pfn;
  unsigned long low_pfn = cc->free_pfn;

  while (pfn > low_pfn && cc->nr_migratepages < 32) {
    struct page *page = phys_to_page(PFN_TO_PHYS(pfn));
    if (!page || PageReserved(page) || PageSlab(page) || PageBuddy(page)) {
      pfn--;
      continue;
    }

    /* Only migrate MOVABLE pages with refcount 1 (not shared) for now */
    if (page->migratetype == MIGRATE_MOVABLE && page_ref_count(page) == 1) {
      list_add(&page->lru, &cc->migratepages);
      cc->nr_migratepages++;
    }
    pfn--;
  }

  cc->migrate_pfn = pfn;
  return cc->nr_migratepages > 0 ? 0 : -1;
}

/**
 * isolate_freepages - Scan zone for free pages to serve as migration targets.
 */
static int isolate_freepages(struct compact_control *cc) {
  unsigned long pfn = cc->free_pfn;
  unsigned long high_pfn = cc->migrate_pfn;

  while (pfn < high_pfn && cc->nr_freepages < cc->nr_migratepages) {
    struct page *page = phys_to_page(PFN_TO_PHYS(pfn));

    /* We need buddy pages (truly free) */
    if (page && PageBuddy(page) && page->order == 0) {
      /*
       * In a real kernel, we would pull this from the buddy allocator
       * to ensure it stays 'isolated'.
       */
      list_add(&page->list, &cc->freepages);
      cc->nr_freepages++;
    }
    pfn++;
  }

  cc->free_pfn = pfn;
  return cc->nr_freepages >= cc->nr_migratepages ? 0 : -1;
}

/**
 * migrate_pages - The core "Move" operation.
 * Physically copies data and updates page tables/RMAP.
 */
static int migrate_pages(struct compact_control *cc) {
  struct list_head *m_pos, *m_tmp;
  struct list_head *f_pos = cc->freepages.next;

  list_for_each_safe(m_pos, m_tmp, &cc->migratepages) {
    struct folio *src_folio = page_folio(list_entry(m_pos, struct page, lru));
    struct page *dst_page = list_entry(f_pos, struct page, list);
    f_pos = f_pos->next;

    /* 1. Unmap from all users */
    if (try_to_unmap_folio(src_folio, nullptr) == 0) {
      /* 2. Copy data */
      void *s_virt = page_address(&src_folio->page);
      void *d_virt = page_address(dst_page);
      memcpy(d_virt, s_virt, PAGE_SIZE);

      /* 3. Swap the page in the object/anon_vma */
      void *mapping = src_folio->mapping;
      uint64_t index = src_folio->index;

      if (mapping) {
        if ((uintptr_t) mapping & 0x1) {
          /* Anonymous - update root anon_vma/object */
        } else {
          struct vm_object *obj = (struct vm_object *) mapping;
          down_write(&obj->lock);
          xa_store(&obj->page_tree, index, dst_page, GFP_ATOMIC);
          up_write(&obj->lock);
        }
      }

      /* 4. Update dst_page metadata to match src */
      dst_page->mapping = mapping;
      dst_page->index = index;
      dst_page->flags = src_folio->page.flags & ~PG_buddy;
      atomic_set(&dst_page->_refcount, 1);

      /* 5. Free old page */
      __free_page(&src_folio->page);
    }

    list_del(m_pos);
    cc->nr_migratepages--;
  }

  /* Clear free list */
  struct list_head *pos, *tmp;
  list_for_each_safe(pos, tmp, &cc->freepages) {
    list_del(pos);
    cc->nr_freepages--;
  }

  return 0;
}

int compact_zone(struct zone *zone, struct compact_control *cc) {
  cc->zone = zone;
  cc->free_pfn = zone->zone_start_pfn;
  cc->migrate_pfn = zone->zone_start_pfn + zone->spanned_pages - 1;

  INIT_LIST_HEAD(&cc->migratepages);
  INIT_LIST_HEAD(&cc->freepages);

  while (cc->migrate_pfn > cc->free_pfn) {
    if (isolate_migratepages(cc) == 0) {
      if (isolate_freepages(cc) == 0) {
        migrate_pages(cc);
      }
    }

    /* Check for signals or preemption in long runs */
    if (unlikely(need_resched)) schedule();

    /* Basic heuristic: stop if we've moved enough or scanners met */
    if (cc->free_pfn > cc->migrate_pfn) break;
  }

  return 0;
}

static int kcompactd_thread(void *data) {
  struct pglist_data *pgdat = (struct pglist_data *) data;
  printk(KERN_INFO PMM_CLASS "kcompactd started for node %d\n", pgdat->node_id);

  while (1) {
    /*
     * In a production system, we wake up when fragmentation is high.
     * For now, we wake up periodically or when kswapd fails.
     */
    delay_ms(5000);

    struct compact_control cc = {
      .order = 9, /* Focus on creating THP (2MB) holes */
      .gfp_mask = GFP_KERNEL,
    };

    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &pgdat->node_zones[i];
      if (z->present_pages > 0) {
        compact_zone(z, &cc);
      }
    }
  }
  return 0;
}

void kcompactd_init(void) {
  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (node_data[n] && node_data[n]->node_spanned_pages > 0) {
      char name[16];
      snprintf(name, sizeof(name), "kcompactd%d", n);
      struct task_struct *k = kthread_create(kcompactd_thread, node_data[n], name);
      if (k) kthread_run(k);
    }
  }
}

unsigned long try_to_compact_pages(gfp_t gfp_mask, unsigned int order) {
  struct compact_control cc = {
    .order = (int) order,
    .gfp_mask = gfp_mask,
  };

  if (order < 2) return 0;

  for (int n = 0; n < MAX_NUMNODES; n++) {
    if (!node_data[n]) continue;
    for (int i = 0; i < MAX_NR_ZONES; i++) {
      struct zone *z = &node_data[n]->node_zones[i];
      if (z->present_pages > 0) compact_zone(z, &cc);
    }
  }
  return 0;
}
