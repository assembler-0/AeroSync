#pragma once

#include <mm/mm_types.h>

/* VMA Operations */

/* initialize the mm_struct */
void mm_init(struct mm_struct *mm);

/* Clean up an mm_struct and all its VMAs */
void mm_destroy(struct mm_struct *mm);

/* Find the VMA containing or immediately following addr */
struct vm_area_struct *vma_find(struct mm_struct *mm, uint64_t addr);

/* Insert a new VMA into the mm structure */
int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma);

/* Allocate a new VMA structure (does not insert it) */
struct vm_area_struct *vma_create(uint64_t start, uint64_t end, uint64_t flags);

/* Free a VMA structure */
void vma_free(struct vm_area_struct *vma);

/* Global kernel mm_struct */
extern struct mm_struct init_mm;

/* Test VMA functionality */
void vma_test(void);

/* Dump VMA info for debugging */
void vma_dump(struct mm_struct *mm);
