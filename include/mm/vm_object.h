#pragma once

#include <mm/mm_types.h>
#include <aerosync/rw_semaphore.h>
#include <aerosync/atomic.h>

typedef enum {
  VM_OBJECT_ANON,
  VM_OBJECT_FILE,
  VM_OBJECT_DEVICE,
  VM_OBJECT_PHYS,
} vm_object_type_t;

/* Object flags */
#define VM_OBJECT_DIRTY         0x01
#define VM_OBJECT_SHADOW        0x02  /* This is a shadow object (has backing) */
#define VM_OBJECT_COLLAPSING    0x04  /* Collapse in progress */
#define VM_OBJECT_DEAD          0x08  /* Object is being destroyed */
#define VM_OBJECT_SWAP_BACKED   0x10  /* Has pages swapped out */

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
 *
 * Shadow Chain Architecture (BSD/XNU-style COW):
 * ───────────────────────────────────────────────
 * When a process forks, both parent and child get shadow objects
 * that point to the original backing object. On write, the shadow
 * copies the page from backing to itself (COW).
 *
 *     Original Object (backing_object)
 *            ↑
 *    ┌───────┴───────┐
 *    │               │
 * Shadow A       Shadow B
 * (parent)       (child)
 *
 * Shadow chains can grow deep with nested forks. We track depth
 * and collapse chains when a shadow's backing has refcount=1.
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

  /* Shadowing and COW support */
  struct vm_object *backing_object; /* For Shadow Objects / COW */
  uint64_t shadow_offset;           /* Offset into the backing object */

  /* Shadow chain management (Phase 1 enhancement) */
  uint16_t shadow_depth;            /* Current depth in shadow chain */
  uint16_t collapse_threshold;      /* Auto-collapse at this depth (default: 8) */
  atomic_t shadow_children;         /* Number of shadows pointing to us */

  /* NUMA affinity */
  int preferred_node;               /* Preferred NUMA node for allocations (-1 = none) */

  /* Page clustering for readahead */
  uint8_t cluster_shift;            /* Readahead window: 1 << cluster_shift pages */

  /* Storage backend properties */
  uint64_t phys_addr; /* For VM_OBJECT_DEVICE and VM_OBJECT_PHYS */
  struct file *file;  /* For VM_OBJECT_FILE */

  /* Statistics */
  atomic_long_t nr_pages;           /* Number of pages in page_tree */
  atomic_long_t nr_swap;            /* Number of pages swapped out */
  
  struct resdomain *rd;             /* Resource domain charging context */
};

#define VM_OBJECT_DIRTY         0x01

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

/* Shadow chain management (Phase 1) */
void vm_object_collapse(struct vm_object *obj);
int vm_object_try_collapse_async(struct vm_object *obj);
int vm_object_shadow_depth(struct vm_object *obj);

/**
 * vm_object_is_shadow - Check if object is a shadow
 */
static inline bool vm_object_is_shadow(struct vm_object *obj) {
    return obj && (obj->flags & VM_OBJECT_SHADOW);
}

/**
 * vm_object_has_swap - Check if object has swapped pages
 */
static inline bool vm_object_has_swap(struct vm_object *obj) {
    return obj && (obj->flags & VM_OBJECT_SWAP_BACKED);
}
