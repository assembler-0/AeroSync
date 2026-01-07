#pragma once

#include <mm/mm_types.h>

typedef enum {
  VM_OBJECT_ANON,
  VM_OBJECT_FILE,
  VM_OBJECT_DEVICE,
  VM_OBJECT_PHYS,
} vm_object_type_t;

struct page_node {
  struct rb_node rb;
  uint64_t pgoff;
  struct page *page;
  int order;
};

/**
 * vm_object_operations - Operations on a VM object
 */
struct vm_object_operations {
  int (*fault)(struct vm_object *obj, struct vm_area_struct *vma, struct vm_fault *vmf);
  void (*free)(struct vm_object *obj);
};

/**
 * struct vm_object - The "Page Cache" anchor.
 * Connects an object (file, anon, device) to its physical pages.
 */
struct vm_object {
  vm_object_type_t type;
  void *priv;               /* Owner/Backing data (e.g., struct inode) */
  struct rb_root page_tree; /* All pages currently in this object */
  spinlock_t lock;
  struct list_head i_mmap;  /* List of all VMAs mapping this object */
  const struct vm_object_operations *ops;
  atomic_t refcount;
  size_t size;
    struct vm_object *backing_object; /* For Shadow Objects / COW */
    uint64_t shadow_offset;           /* Offset into the backing object */

    uint64_t phys_addr; /* For VM_OBJECT_DEVICE and VM_OBJECT_PHYS */
};

/* API */
struct vm_object *vm_object_alloc(vm_object_type_t type);
void vm_object_free(struct vm_object *obj);
void vm_object_get(struct vm_object *obj);
void vm_object_put(struct vm_object *obj);

/* Page management */
int vm_object_add_page(struct vm_object *obj, uint64_t pgoff, struct page *page, struct page_node *node);
struct page *vm_object_find_page(struct vm_object *obj, uint64_t pgoff);
void vm_object_remove_page(struct vm_object *obj, uint64_t pgoff);

/* Helpers */
struct vm_object *vm_object_anon_create(size_t size);
struct vm_object *vm_object_device_create(uint64_t phys_addr, size_t size);
struct vm_object *vm_object_shadow_create(struct vm_object *backing, uint64_t offset, size_t size);
int vm_object_cow_prepare(struct vm_area_struct *vma, struct vm_area_struct *new_vma);
