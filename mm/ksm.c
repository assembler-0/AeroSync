/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/ksm.c
 * @brief Kernel Samepage Merging (KSM) Daemon
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/sched/process.h>
#include <aerosync/spinlock.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <mm/ksm.h>
#include <mm/slub.h>
#include <mm/vma.h>

/* Global KSM configuration */
static unsigned int ksm_sleep_ms = CONFIG_MM_KSM_SLEEP_MSEC;
static unsigned int ksm_pages_to_scan = CONFIG_MM_KSM_PAGES_TO_SCAN;
static bool ksm_run = true;

struct ksm_mm_slot {
  struct list_head mm_list;
  struct mm_struct *mm;
};

struct ksm_node {
  struct rb_node node;
  uint32_t hash;
  uint64_t phys;
  struct mm_struct *mm;
  uint64_t virt;
};

static DEFINE_SPINLOCK(ksm_mmlist_lock);
static LIST_HEAD(ksm_mm_head);
static struct rb_root ksm_stable_tree = RB_ROOT;
static struct rb_root ksm_unstable_tree = RB_ROOT;

static uint32_t calculate_page_hash(const void *addr) {
  uint32_t hash = 5381;
  const uint8_t *p = (const uint8_t *)addr;
  for (int i = 0; i < PAGE_SIZE; i++) {
    hash = ((hash << 5) + hash) + p[i];
  }
  return hash;
}

static int ksm_cmp(uint32_t h1, uint64_t p1, uint32_t h2, uint64_t p2) {
  if (h1 < h2)
    return -1;
  if (h1 > h2)
    return 1;
  /* If hashes match, perform a full byte-by-byte comparison */
  return memcmp(pmm_phys_to_virt(p1), pmm_phys_to_virt(p2), PAGE_SIZE);
}

static struct ksm_node *ksm_tree_search(struct rb_root *root, uint32_t hash,
                                        uint64_t phys) {
  struct rb_node *node = root->rb_node;
  while (node) {
    struct ksm_node *knode = rb_entry(node, struct ksm_node, node);
    int cmp = ksm_cmp(hash, phys, knode->hash, knode->phys);
    if (cmp < 0)
      node = node->rb_left;
    else if (cmp > 0)
      node = node->rb_right;
    else
      return knode;
  }
  return nullptr;
}

static void ksm_tree_insert(struct rb_root *root, struct ksm_node *knode) {
  struct rb_node **new = &(root->rb_node), *parent = nullptr;
  while (*new) {
    struct ksm_node *this = rb_entry(*new, struct ksm_node, node);
    parent = *new;
    int cmp = ksm_cmp(knode->hash, knode->phys, this->hash, this->phys);
    if (cmp < 0)
      new = &((*new)->rb_left);
    else
      new = &((*new)->rb_right);
  }
  rb_link_node(&knode->node, parent, new);
  rb_insert_color(&knode->node, root);
}

static void ksm_merge_page(struct mm_struct *mm, uint64_t virt,
                           uint64_t old_phys, uint64_t new_phys) {
  /* Mark it read-only to trigger COW on write by clearing PTE_RW */
  vmm_unmap_page(mm, virt);
  /* Map with new physical page, marking it read-only (omitting PTE_RW) */
  vmm_map_page(mm, virt, new_phys, PTE_PRESENT | PTE_USER);

  if (old_phys != new_phys) {
    struct page *pg = phys_to_page(old_phys);
    if (pg)
      put_page(pg);
  }

  printk(KERN_DEBUG KSM_CLASS
         "Merged duplicate page at %llx (mm %p) to shared phys %llx\n",
         (unsigned long long)virt, (void *)mm, (unsigned long long)new_phys);
}

int ksm_madvise(struct vm_area_struct *vma, uint64_t start, uint64_t end,
                int advice) {
  (void)start;
  (void)end;
  if (!vma->vm_mm)
    return 0;

  if (advice == MADV_MERGEABLE) {
    vma->vm_flags |= VM_MERGEABLE;
    ksm_enter(vma->vm_mm);
  }

  return 0;
}

int ksm_enter(struct mm_struct *mm) {
  struct ksm_mm_slot *slot;
  bool found = false;

  irq_flags_t flags = spinlock_lock_irqsave(&ksm_mmlist_lock);
  list_for_each_entry(slot, &ksm_mm_head, mm_list) {
    if (slot->mm == mm) {
      found = true;
      break;
    }
  }
  spinlock_unlock_irqrestore(&ksm_mmlist_lock, flags);

  if (found)
    return 0;

  slot = kzalloc(sizeof(struct ksm_mm_slot));
  if (!slot)
    return -ENOMEM;

  slot->mm = mm;
  atomic_inc(&mm->mm_count);

  flags = spinlock_lock_irqsave(&ksm_mmlist_lock);
  list_add_tail(&slot->mm_list, &ksm_mm_head);
  spinlock_unlock_irqrestore(&ksm_mmlist_lock, flags);

  return 0;
}

static void ksm_do_scan(unsigned int pages_to_scan) {
  struct ksm_mm_slot *slot;
  unsigned int scanned = 0;

  irq_flags_t flags = spinlock_lock_irqsave(&ksm_mmlist_lock);
  if (list_empty(&ksm_mm_head)) {
    spinlock_unlock_irqrestore(&ksm_mmlist_lock, flags);
    return;
  }
  slot = list_first_entry(&ksm_mm_head, struct ksm_mm_slot, mm_list);
  list_del(&slot->mm_list);
  list_add_tail(&slot->mm_list, &ksm_mm_head);
  spinlock_unlock_irqrestore(&ksm_mmlist_lock, flags);

  struct mm_struct *mm = slot->mm;

  if (down_read_trylock(&mm->mmap_lock)) {
    struct vm_area_struct *vma;
    for_each_vma(mm, vma) {
      if (!(vma->vm_flags & VM_MERGEABLE))
        continue;

      for (uint64_t addr = vma->vm_start;
           addr < vma->vm_end && scanned < pages_to_scan; addr += PAGE_SIZE) {
        uint64_t phys = vmm_virt_to_phys(mm, addr);
        if (!phys)
          continue;

        uint32_t hash = calculate_page_hash(pmm_phys_to_virt(phys));
        scanned++;

        struct ksm_node *found = ksm_tree_search(&ksm_stable_tree, hash, phys);
        if (found) {
          if (found->phys != phys) {
            ksm_merge_page(mm, addr, phys, found->phys);
          }
          continue;
        }

        found = ksm_tree_search(&ksm_unstable_tree, hash, phys);
        if (found) {
          rb_erase(&found->node, &ksm_unstable_tree);
          ksm_tree_insert(&ksm_stable_tree, found);
          ksm_merge_page(mm, addr, phys, found->phys);
        } else {
          struct ksm_node *knode = kmalloc(sizeof(struct ksm_node));
          if (knode) {
            knode->hash = hash;
            knode->phys = phys;
            knode->mm = mm;
            knode->virt = addr;
            ksm_tree_insert(&ksm_unstable_tree, knode);
          }
        }
      }
      if (scanned >= pages_to_scan)
        break;
    }
    up_read(&mm->mmap_lock);
  }

  static int cycles = 0;
  if (++cycles > 10) {
    cycles = 0;
    struct rb_node *node;
    while ((node = rb_first(&ksm_unstable_tree))) {
      struct ksm_node *knode = rb_entry(node, struct ksm_node, node);
      rb_erase(node, &ksm_unstable_tree);
      kfree(knode);
    }
  }
}

static int ksm_thread(void *data) {
  (void)data;
  printk(KERN_INFO KSM_CLASS "KSM Daemon started\n");

  while (ksm_run) {
    schedule_timeout(ksm_sleep_ms * 1000000ULL);
    ksm_do_scan(ksm_pages_to_scan);
  }

  return 0;
}

int ksm_init(void) {
  struct task_struct *task = kthread_create(ksm_thread, nullptr, "ksmd");
  if (!task) {
    printk(KERN_ERR KSM_CLASS "Failed to start KSM daemon\n");
    return -ENOMEM;
  }
  kthread_run(task);
  return 0;
}
