#pragma once

#include <mm/mm_types.h>

/* VMA Flags */
#define VM_READ         0x00000001
#define VM_WRITE        0x00000002
#define VM_EXEC         0x00000004
#define VM_SHARED       0x00000008
#define VM_MAYREAD      0x00000010
#define VM_MAYWRITE     0x00000020
#define VM_MAYEXEC      0x00000040
#define VM_MAYSHARE     0x00000080
#define VM_GROWSDOWN    0x00000100
#define VM_GROWSUP      0x00000200
#define VM_IO           0x00004000  /* Memory mapped I/O */
#define VM_DONTCOPY     0x00020000  /* Don't copy on fork */
#define VM_DONTEXPAND   0x00040000  /* Cannot expand with mremap */
#define VM_LOCKED       0x00100000  /* Pages are locked */
#define VM_PFNMAP       0x00400000  /* Physical frame number mapping */

/* VMA merge flags */
#define VMA_MERGE_PREV  0x1
#define VMA_MERGE_NEXT  0x2

/* MM Operations */
void mm_init(struct mm_struct *mm);
void mm_destroy(struct mm_struct *mm);
struct mm_struct *mm_alloc(void);
void mm_free(struct mm_struct *mm);

/* VMA Operations */
struct vm_area_struct *vma_alloc(void);
void vma_free(struct vm_area_struct *vma);
struct vm_area_struct *vma_create(uint64_t start, uint64_t end, uint64_t flags);

/* VMA Lookup */
struct vm_area_struct *vma_find(struct mm_struct *mm, uint64_t addr);
struct vm_area_struct *vma_find_exact(struct mm_struct *mm, uint64_t start, uint64_t end);
struct vm_area_struct *vma_find_intersection(struct mm_struct *mm, uint64_t start, uint64_t end);

/* VMA Modification */
int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma);
void vma_remove(struct mm_struct *mm, struct vm_area_struct *vma);
int vma_split(struct mm_struct *mm, struct vm_area_struct *vma, uint64_t addr);
int vma_merge(struct mm_struct *mm, struct vm_area_struct *vma);
int vma_expand(struct mm_struct *mm, struct vm_area_struct *vma,
               uint64_t new_start, uint64_t new_end);
int vma_shrink(struct mm_struct *mm, struct vm_area_struct *vma,
               uint64_t new_start, uint64_t new_end);

/* VMA Allocation */
uint64_t vma_find_free_region(struct mm_struct *mm, size_t size,
                              uint64_t range_start, uint64_t range_end);
uint64_t vma_find_free_region_aligned(struct mm_struct *mm, size_t size,
                                     uint64_t alignment,
                                     uint64_t range_start, uint64_t range_end);

/* High-level VMA operations */
int vma_map_range(struct mm_struct *mm, uint64_t start, uint64_t end, uint64_t flags);
int vma_unmap_range(struct mm_struct *mm, uint64_t start, uint64_t end);
int vma_protect(struct mm_struct *mm, uint64_t start, uint64_t end, uint64_t new_flags);

/* VMA Iteration */
struct vm_area_struct *vma_next(struct vm_area_struct *vma);
struct vm_area_struct *vma_prev(struct vm_area_struct *vma);

/* Statistics and Debugging */
void vma_dump(struct mm_struct *mm);
void vma_dump_single(struct vm_area_struct *vma);
size_t mm_total_size(struct mm_struct *mm);
size_t mm_map_count(struct mm_struct *mm);

/* Validation */
int vma_verify_tree(struct mm_struct *mm);
int vma_verify_list(struct mm_struct *mm);

/* Global kernel mm_struct */
extern struct mm_struct init_mm;

/* Test VMA functionality */
void vma_test(void);

/* VMA Cache for performance */
void vma_cache_init(void);
struct vm_area_struct *vma_cache_alloc(void);
void vma_cache_free(struct vm_area_struct *vma);

/* Helper macros */
#define vma_pages(vma) (((vma)->vm_end - (vma)->vm_start) >> PAGE_SHIFT)
#define vma_size(vma) ((vma)->vm_end - (vma)->vm_start)

#define for_each_vma(mm, vma) \
    list_for_each_entry(vma, &(mm)->mmap_list, vm_list)

#define for_each_vma_safe(mm, vma, tmp) \
    list_for_each_entry_safe(vma, tmp, &(mm)->mmap_list, vm_list)