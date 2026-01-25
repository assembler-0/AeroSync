#pragma once

#include <mm/mm_types.h>
#include <aerosync/rw_semaphore.h>

typedef enum {
  VM_OBJECT_ANON,
  VM_OBJECT_FILE,
  VM_OBJECT_DEVICE,
  VM_OBJECT_PHYS,
} vm_object_type_t;

/**
 * vm_object_operations - Operations on a VM object
 */
struct folio;
struct page;
struct vm_object_operations {
  int (*fault)(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf);
  int (*page_mkwrite)(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf);
  int (*read_folio)(struct vm_object *obj, struct folio *folio);
  int (*write_folio)(struct vm_object *obj, struct folio *folio);
  void (*free)(struct vm_object *obj);
};

#include <linux/xarray.h>

/**
 * struct vm_object - The "Page Cache" anchor.
 * Connects an object (file, anon, device) to its physical pages.
 */
struct vm_object {
  vm_object_type_t type;
  void *priv;               /* Owner/Backing data (e.g., struct inode) */
  struct xarray page_tree;  /* All pages currently in this object (indexed by pgoff) */
  struct rw_semaphore lock;
  struct list_head i_mmap;  /* List of all VMAs mapping this object */
  struct list_head dirty_list; /* Node in global dirty_objects list */
  const struct vm_object_operations *ops;
  atomic_t refcount;
  uint32_t flags;
  size_t size;
  struct vm_object *backing_object; /* For Shadow Objects / COW */
  uint64_t shadow_offset;           /* Offset into the backing object */

  uint64_t phys_addr; /* For VM_OBJECT_DEVICE and VM_OBJECT_PHYS */
};

#define VM_OBJECT_DIRTY 0x01

/* API */
struct vm_object *vm_object_alloc(vm_object_type_t type);
void vm_object_free(struct vm_object *obj);
void vm_object_get(struct vm_object *obj);
void vm_object_put(struct vm_object *obj);

/* Writeback and Throttling */
void vm_object_mark_dirty(struct vm_object *obj);
void vm_writeback_init(void);
void balance_dirty_pages(struct vm_object *obj);
void wakeup_writeback(void);
void account_page_dirtied(void);
void account_page_cleaned(void);

/* Page/Folio management */
int vm_object_add_folio(struct vm_object *obj, uint64_t pgoff, struct folio *folio);
struct folio *vm_object_find_folio(struct vm_object *obj, uint64_t pgoff);
void vm_object_remove_folio(struct vm_object *obj, uint64_t pgoff);

/* Deprecated helpers (prefer folio versions) */
int vm_object_add_page(struct vm_object *obj, uint64_t pgoff, struct page *page);
struct page *vm_object_find_page(struct vm_object *obj, uint64_t pgoff);
void vm_object_remove_page(struct vm_object *obj, uint64_t pgoff);

/* Helpers */
struct vm_object *vm_object_anon_create(size_t size);
struct vm_object *vm_object_device_create(uint64_t phys_addr, size_t size);
struct vm_object *vm_object_shadow_create(struct vm_object *backing, uint64_t offset, size_t size);
int vm_object_cow_prepare(struct vm_area_struct *vma, struct vm_area_struct *new_vma);
