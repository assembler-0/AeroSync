/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/filemap.c
 * @brief Page Cache / File Mapping implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <mm/vm_object.h>
#include <mm/vma.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <fs/vfs.h>
#include <aerosync/errno.h>
#include <lib/string.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/uaccess.h>
#include <aerosync/fkx/fkx.h>

/**
 * do_page_cache_readahead - Proactively read neighboring pages into cache.
 */
static void do_page_cache_readahead(struct vm_object *obj, uint64_t pgoff) {
#ifdef CONFIG_MM_READAHEAD
  uint32_t count = 1 << obj->cluster_shift;
  if (count > CONFIG_MM_READAHEAD_PAGES) count = CONFIG_MM_READAHEAD_PAGES;

  for (uint32_t i = 1; i < count; i++) {
    uint64_t next_off = pgoff + i;

    /* Check if already in cache */
    down_read(&obj->lock);
    struct folio *exists = vm_object_find_folio(obj, next_off);
    up_read(&obj->lock);
    if (exists) continue;

    /* Check EOF */
    if (obj->size && (next_off << PAGE_SHIFT) >= obj->size) break;

    struct folio *folio = alloc_pages(GFP_KERNEL, 0);
    if (!folio) break;

    /* Read asynchronously if possible, for now synchronous */
    if (obj->ops && obj->ops->read_folio) {
      if (obj->ops->read_folio(obj, folio) < 0) {
        folio_put(folio);
        break;
      }
    } else {
      memset(folio_address(folio), 0, PAGE_SIZE);
    }

    down_write(&obj->lock);
    /* Double check race */
    if (vm_object_find_folio(obj, next_off)) {
      up_write(&obj->lock);
      folio_put(folio);
      continue;
    }

    if (vm_object_add_folio(obj, next_off, folio) < 0) {
      up_write(&obj->lock);
      folio_put(folio);
      break;
    }
    up_write(&obj->lock);

    /* RMAP linkage for readahead pages */
    folio_add_file_rmap(folio, obj, next_off);
  }
#else
  (void) obj;
  (void) pgoff;
#endif
}

/**
 * filemap_fault - The default fault handler for file-backed objects.
 */
int filemap_fault(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  struct folio *folio;
  int ret;

  down_read(&obj->lock);

  /* 1. Check if folio is already in cache */
  folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    folio_get(folio);
    vmf->folio = folio;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    up_read(&obj->lock);
    return 0;
  }

  up_read(&obj->lock);

  /* 2. Cache miss: Trigger readahead for subsequent pages */
  do_page_cache_readahead(obj, vmf->pgoff);

  /* 3. Allocate a new folio for the current fault */
  folio = alloc_pages(GFP_KERNEL, 0);
  if (!folio) return VM_FAULT_OOM;

  /* 4. Read data from filesystem into the folio */
  if (obj->ops && obj->ops->read_folio) {
    ret = obj->ops->read_folio(obj, folio);
    if (ret < 0) {
      folio_put(folio);
      return VM_FAULT_SIGBUS;
    }
  } else {
    /* No read_folio provided, zero fill (should not happen for files) */
    memset(folio_address(folio), 0, PAGE_SIZE);
  }

  /* 5. Insert into page cache */
  down_write(&obj->lock);

  /* Re-check race */
  struct folio *existing = vm_object_find_folio(obj, vmf->pgoff);
  if (existing) {
    up_write(&obj->lock);
    folio_put(folio);
    folio_get(existing);
    vmf->folio = existing;
    vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
    return 0;
  }

  if (vm_object_add_folio(obj, vmf->pgoff, folio) < 0) {
    up_write(&obj->lock);
    folio_put(folio);
    return VM_FAULT_SIGBUS;
  }

  folio_get(folio);
  vmf->folio = folio;
  vmf->prot = vma ? vma->vm_page_prot : vm_get_page_prot(VM_READ);
  up_write(&obj->lock);

  /* File-backed RMAP linkage */
  folio_add_file_rmap(folio, obj, vmf->pgoff);

  return 0;
}

/**
 * filemap_page_mkwrite - Default mkwrite for files.
 * Marks the folio as dirty and prepares for writeback.
 */
int filemap_page_mkwrite(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf) {
  (void) vma;
  down_write(&obj->lock);
  struct folio *folio = vm_object_find_folio(obj, vmf->pgoff);
  if (folio) {
    if (!(folio->page.flags & PG_dirty)) {
      folio->page.flags |= PG_dirty;
      account_page_dirtied();
    }
  }
  up_write(&obj->lock);

  /* Queue for background writeback */
  vm_object_mark_dirty(obj);

  /* Prevent process from running away with dirty memory */
  balance_dirty_pages(obj);

  return 0;
}

const struct vm_object_operations filemap_obj_ops = {
  .fault = filemap_fault,
  .page_mkwrite = filemap_page_mkwrite,
};

/**
 * generic_file_mmap - Sets up a VMA for file mapping.
 */
int generic_file_mmap(struct file *file, struct vm_area_struct *vma) {
  struct inode *inode = file->f_inode;

  /* Ensure inode has a mapping (vm_object) */
  if (!inode->i_mapping) {
    /*
     * In a real kernel, this is usually initialized when the inode is loaded.
     * For now, we lazily create it if it's missing.
     */
    inode->i_mapping = vm_object_alloc(VM_OBJECT_FILE);
    if (!inode->i_mapping) return -ENOMEM;
    inode->i_mapping->priv = inode;
    inode->i_mapping->size = inode->i_size;
    inode->i_mapping->ops = &filemap_obj_ops;
  }

  vma->vm_obj = inode->i_mapping;
  vm_object_get(vma->vm_obj);

  /* Link VMA to the mapping for RMAP */
  down_write(&vma->vm_obj->lock);
  list_add(&vma->vm_shared, &vma->vm_obj->i_mmap);
  up_write(&vma->vm_obj->lock);

  return 0;
}

/**
 * filemap_read - Reads from the page cache into a user buffer.
 */
ssize_t filemap_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct inode *inode = file->f_inode;
  struct vm_object *obj = inode->i_mapping;
  size_t total_read = 0;

  if (!obj) return -EINVAL;

  while (count > 0) {
    uint64_t pgoff = (*ppos) >> PAGE_SHIFT;
    size_t offset = (*ppos) & (PAGE_SIZE - 1);
    size_t n = PAGE_SIZE - offset;
    if (n > count) n = count;

    /* Check EOF */
    if (*ppos >= inode->i_size) break;
    if (*ppos + n > inode->i_size) n = inode->i_size - *ppos;

    struct vm_fault vmf = {
      .pgoff = pgoff,
      .flags = 0,
    };

    /* Force fault to bring page into cache */
    int ret = obj->ops->fault(obj, nullptr, &vmf);
    if (ret != 0) return total_read ? (ssize_t) total_read : -EIO;

    void *kaddr = folio_address(vmf.folio);
    if (file->f_mode & FMODE_KERNEL) {
      memcpy(buf, kaddr + offset, n);
    } else {
      copy_to_user(buf, kaddr + offset, n);
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
 * filemap_write - Writes from a user buffer into the page cache.
 */
ssize_t filemap_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  struct inode *inode = file->f_inode;
  struct vm_object *obj = inode->i_mapping;
  size_t total_written = 0;

  if (!obj) return -EINVAL;

  while (count > 0) {
    uint64_t pgoff = (*ppos) >> PAGE_SHIFT;
    size_t offset = (*ppos) & (PAGE_SIZE - 1);
    size_t n = PAGE_SIZE - offset;
    if (n > count) n = count;

    struct vm_fault vmf = {
      .pgoff = pgoff,
      .flags = FAULT_FLAG_WRITE,
    };

    /*
     * Force fault to bring page into cache.
     * handle_mm_fault would normally be called here if we had a VMA,
     * but for direct VFS write we call the object's fault handler directly.
     */
    int ret = obj->ops->fault(obj, nullptr, &vmf);
    if (ret != 0) return total_written ? (ssize_t) total_written : -EIO;

    struct folio *folio = vmf.folio;
    void *kaddr = folio_address(folio);

    /* Copy from user or kernel into the cache */
    if (file->f_mode & FMODE_KERNEL) {
      memcpy(kaddr + offset, buf, n);
    } else {
      if (copy_from_user(kaddr + offset, buf, n) != 0) {
        folio_put(folio);
        return total_written ? (ssize_t) total_written : -EFAULT;
      }
    }

    /* Mark dirty and prepare for writeback */
    down_write(&obj->lock);
    if (!(folio->page.flags & PG_dirty)) {
      folio->page.flags |= PG_dirty;
      account_page_dirtied();
    }
    up_write(&obj->lock);

    vm_object_mark_dirty(obj);

    folio_put(folio);

    /* Update inode size if we wrote past end */
    if (*ppos + n > inode->i_size) {
      inode->i_size = *ppos + n;
      obj->size = inode->i_size;
    }

    buf += n;
    count -= n;
    *ppos += n;
    total_written += n;

    balance_dirty_pages(obj);
  }

  return (ssize_t) total_written;
}

EXPORT_SYMBOL(filemap_read);
EXPORT_SYMBOL(filemap_write);
EXPORT_SYMBOL(generic_file_mmap);
