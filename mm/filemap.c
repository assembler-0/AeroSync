/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/filemap.c
 * @brief Advanced Unified Buffer Cache (UBC) & Page Cache implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <mm/vm_object.h>
#include <mm/vma.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <fs/vfs.h>
#include <aerosync/errno.h>
#include <lib/string.h>
#include <lib/math.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/uaccess.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/resdomain.h>

/**
 * ubc_readahead - Advanced adaptive readahead logic.
 *
 * Implements a sliding window readahead algorithm similar to Linux.
 * It tracks sequential access patterns and grows/shrinks the window.
 */
static void ubc_readahead(struct vm_object *obj, uint64_t pgoff) {
#ifdef CONFIG_MM_READAHEAD
  unsigned long ra_start = obj->readahead.start;
  unsigned int ra_size = obj->readahead.size;

  if (pgoff == ra_start + ra_size) {
    /* Sequential hit - double window */
#ifdef CONFIG_MM_READAHEAD_THRASH_PROTECTION
    if (obj->readahead.thrash_count > 5) {
      obj->readahead.size = min(ra_size + 1, (unsigned int)obj->readahead.ra_pages);
    } else
#endif
    {
      if (ra_size == 0) obj->readahead.size = 4;
      else obj->readahead.size = min(ra_size * 2, (unsigned int)obj->readahead.ra_pages);
    }
    obj->readahead.start = pgoff;
  } else {
    /* Random hit - reset window */
    obj->readahead.size = 4;
    obj->readahead.start = pgoff;
  }

  /* Scaling: if system-wide free pages are low, cap readahead */
  extern unsigned long nr_free_pages(void);
  if (nr_free_pages() < 1024) {
    /* 4MB threshold */
    obj->readahead.size = min(obj->readahead.size, 4U);
  }

  uint32_t count = obj->readahead.size;
  for (uint32_t i = 1; i <= count; i++) {
    uint64_t next_off = pgoff + i;

    /* Check EOF */
    if (obj->size && (next_off << PAGE_SHIFT) >= obj->size) break;

    /* Fast check if already present */
    if (vm_object_find_folio(obj, next_off)) continue;

    struct folio *folio = alloc_pages_node(obj->preferred_node, GFP_KERNEL, 0);
    if (!folio) break;

    /* UBC Charging */
    if (obj->rd && resdomain_charge_mem(obj->rd, PAGE_SIZE, false) < 0) {
      folio_put(folio);
      break;
    }
    folio->page.rd = obj->rd;

    if (obj->ops && obj->ops->read_folio) {
      if (obj->ops->read_folio(obj, folio) < 0) {
        if (obj->rd) resdomain_uncharge_mem(obj->rd, PAGE_SIZE);
        folio_put(folio);
        break;
      }
    } else {
      memset(folio_address(folio), 0, PAGE_SIZE);
    }

    down_write(&obj->lock);
    if (vm_object_find_folio(obj, next_off)) {
      up_write(&obj->lock);
      if (obj->rd) resdomain_uncharge_mem(obj->rd, PAGE_SIZE);
      folio_put(folio);
      continue;
    }

    if (vm_object_add_folio(obj, next_off, folio) < 0) {
      up_write(&obj->lock);
      if (obj->rd) resdomain_uncharge_mem(obj->rd, PAGE_SIZE);
      folio_put(folio);
      break;
    }
    atomic_long_inc(&obj->nr_pages);
    up_write(&obj->lock);

    folio_add_file_rmap(folio, obj, next_off);
  }
#else
  (void) obj; (void) pgoff;
#endif
}

/**
 * filemap_fault - Production-grade fault handler for UBC.
 */
int filemap_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  struct folio *folio;
  int ret;

  /* Sanity Check for poisoned object */
  if (unlikely((uintptr_t)obj == 0xadadadadadadadad || (uintptr_t)obj == 0xdeadbeefcafebabe))
    return VM_FAULT_SIGBUS;

  /* 1. Fast path: check cache under RCU/Shared lock */
  down_read(&obj->lock);
  folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio && !xa_is_err(folio)) {
    /* Sanity check for poisoned folio */
    if (unlikely((uintptr_t)folio == 0xadadadadadadadad || (uintptr_t)folio == 0xdeadbeefcafebabe)) {
        up_read(&obj->lock);
        return VM_FAULT_SIGBUS;
    }
    folio_get(folio);
    vmf->folio = folio;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    if (!vma && (vmf->flags & FAULT_FLAG_WRITE)) vmf->prot |= PTE_RW;
    if (vma && !(vma->vm_flags & VM_SHARED)) vmf->prot &= ~PTE_RW;
    up_read(&obj->lock);
    return 0;
  }
  up_read(&obj->lock);

  /* 2. Bounds check - use rounded up size to support partial pages */
  if (vmf->pgoff >= ((obj->size + PAGE_SIZE - 1) >> PAGE_SHIFT)) {
    return VM_FAULT_SIGSEGV;
  }

  /* 3. Slow path: Readahead and Read */
  ubc_readahead(obj, vmf->pgoff);

  /* Allocate for current fault */
  int nid = vma ? vma->preferred_node : obj->preferred_node;
  if (nid == -1) nid = this_node();
  folio = alloc_pages_node(nid, GFP_KERNEL, 0);
  if (!folio) return VM_FAULT_OOM;

  /* Charge to ResDomain */
  struct resdomain *rd = obj->rd ? obj->rd : (vma && vma->vm_mm ? vma->vm_mm->rd : nullptr);
  if (rd && resdomain_charge_mem(rd, PAGE_SIZE, false) < 0) {
    folio_put(folio);
    return VM_FAULT_OOM;
  }
  folio->page.rd = rd;

  if (obj->ops && obj->ops != (void*)0xdeadbeefcafebabe && obj->ops->read_folio) {
    ret = obj->ops->read_folio(obj, folio);
    if (ret < 0) {
      if (rd) resdomain_uncharge_mem(rd, PAGE_SIZE);
      folio_put(folio);
      return VM_FAULT_SIGBUS;
    }
  } else {
    memset(folio_address(folio), 0, PAGE_SIZE);
  }

  down_write(&obj->lock);
  struct folio *existing = vm_object_find_folio(obj, vmf->pgoff);
  if (existing && !xa_is_err(existing)) {
    up_write(&obj->lock);
    if (rd) resdomain_uncharge_mem(rd, PAGE_SIZE);
    folio_put(folio);
    
    if (unlikely((uintptr_t)existing == 0xadadadadadadadad || (uintptr_t)existing == 0xdeadbeefcafebabe))
        return VM_FAULT_SIGBUS;

    folio_get(existing);
    vmf->folio = existing;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    if (!vma && (vmf->flags & FAULT_FLAG_WRITE)) vmf->prot |= PTE_RW;
    return 0;
  }

  if (vm_object_add_folio(obj, vmf->pgoff, folio) < 0) {
    up_write(&obj->lock);
    if (rd) resdomain_uncharge_mem(rd, PAGE_SIZE);
    folio_put(folio);
    return VM_FAULT_SIGBUS;
  }
  atomic_long_inc(&obj->nr_pages);
  vmf->folio = folio;
  folio_get(folio);
  vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
  if (!vma && (vmf->flags & FAULT_FLAG_WRITE)) vmf->prot |= PTE_RW;
  if (vma && !(vma->vm_flags & VM_SHARED)) vmf->prot &= ~PTE_RW;
  up_write(&obj->lock);

  folio_add_file_rmap(folio, obj, vmf->pgoff);
  return 0;
}

int filemap_page_mkwrite(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  (void) vma;
  down_write(&obj->lock);
  struct folio *folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    if (!(folio->page.flags & PG_dirty)) {
      folio->page.flags |= PG_dirty;
      account_page_dirtied();
      vm_object_mark_dirty(obj);
    }
  }
  up_write(&obj->lock);
  balance_dirty_pages(obj);
  return 0;
}

const struct vm_object_operations vnode_ubc_ops = {
  .fault = filemap_fault,
  .page_mkwrite = filemap_page_mkwrite,
};

/**
 * filemap_read - Optimized batched UBC read.
 */
ssize_t filemap_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct inode *inode = file->f_inode;
  struct vm_object *obj = inode->i_ubc;
  size_t total_read = 0;

  if (!obj) return -EINVAL;

  /* UBC Sequential Access Hint */
  ubc_readahead(obj, (*ppos) >> PAGE_SHIFT);

  while (count > 0) {
    uint64_t pgoff = (*ppos) >> PAGE_SHIFT;
    size_t offset = (*ppos) & (PAGE_SIZE - 1);
    size_t n = min(count, PAGE_SIZE - offset);

    if (*ppos >= inode->i_size) break;
    if (*ppos + n > inode->i_size) n = inode->i_size - *ppos;

    struct vm_fault vmf = {.pgoff = pgoff, .flags = 0};
    int ret = filemap_fault(obj, nullptr, &vmf);
    if (ret != 0) return total_read ? (ssize_t) total_read : -EIO;

    void *kaddr = pmm_phys_to_virt(folio_to_phys(vmf.folio));
    if (file->f_mode & FMODE_KERNEL) memcpy(buf, kaddr + offset, n);
    else if (copy_to_user(buf, kaddr + offset, n) != 0) {
      folio_put(vmf.folio);
      return total_read ? (ssize_t) total_read : -EFAULT;
    }

    folio_put(vmf.folio);
    buf += n;
    count -= n;
    *ppos += n;
    total_read += n;
  }
  return (ssize_t) total_read;
}

/**
 * filemap_write - Optimized batched UBC write with throttling.
 */
ssize_t filemap_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  struct inode *inode = file->f_inode;
  struct vm_object *obj = inode->i_ubc;
  size_t total_written = 0;

  if (!obj) return -EINVAL;

  while (count > 0) {
    uint64_t pgoff = (*ppos) >> PAGE_SHIFT;
    size_t offset = (*ppos) & (PAGE_SIZE - 1);
    size_t n = min(count, PAGE_SIZE - offset);

    /* Extend file size if writing beyond EOF */
    if (*ppos + n > inode->i_size) {
        inode->i_size = *ppos + n;
        obj->size = inode->i_size;
    }

    struct vm_fault vmf = {.pgoff = pgoff, .flags = FAULT_FLAG_WRITE};
    int ret = filemap_fault(obj, nullptr, &vmf);
    if (ret != 0) return total_written ? (ssize_t) total_written : -EIO;

    struct folio *folio = vmf.folio;
    void *kaddr = pmm_phys_to_virt(folio_to_phys(folio));

    if (file->f_mode & FMODE_KERNEL) memcpy(kaddr + offset, buf, n);
    else if (copy_from_user(kaddr + offset, buf, n) != 0) {
      folio_put(folio);
      return total_written ? (ssize_t) total_written : -EFAULT;
    }

    down_write(&obj->lock);
    if (!(folio->page.flags & PG_dirty)) {
      folio->page.flags |= PG_dirty;
      account_page_dirtied();
      vm_object_mark_dirty(obj);
    }
    up_write(&obj->lock);

    folio_put(folio);

    buf += n;
    count -= n;
    *ppos += n;
    total_written += n;

    balance_dirty_pages(obj);
  }
  return (ssize_t) total_written;
}

int generic_file_mmap(struct file *file, struct vm_area_struct *vma) {
  struct inode *inode = file->f_inode;
  if (!inode->i_ubc) {
    inode->i_ubc = vm_object_alloc(VM_OBJECT_VNODE);
    if (!inode->i_ubc) return -ENOMEM;
    inode->i_ubc->vnode = inode;
    inode->i_ubc->size = inode->i_size;
    inode->i_ubc->ops = &vnode_ubc_ops;
  }
  vma->vm_obj = inode->i_ubc;
  vm_object_get(vma->vm_obj);
  vma_obj_node_setup(vma);
  down_write(&vma->vm_obj->lock);
  interval_tree_insert(&vma->obj_node, &vma->vm_obj->i_mmap);
  up_write(&vma->vm_obj->lock);
  return 0;
}

EXPORT_SYMBOL(filemap_read);
EXPORT_SYMBOL(filemap_write);
EXPORT_SYMBOL(generic_file_mmap);

void *ubc_map_page(struct vm_object *obj, uint64_t pgoff) {
  struct vm_fault vmf = {.pgoff = pgoff, .flags = 0};
  int ret = filemap_fault(obj, nullptr, &vmf);
  if (ret != 0) return nullptr;
  return pmm_phys_to_virt(folio_to_phys(vmf.folio));
}

void ubc_unmap_page(struct folio *folio) {
  if (folio) folio_put(folio);
}
