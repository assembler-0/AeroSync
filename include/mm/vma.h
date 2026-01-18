#pragma once

#include <mm/mm_types.h>

/* Prot flags */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* Map flags */
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANON    0x20
#define MAP_STACK   0x40
#define MAP_LOCKED  0x80

/* VMA merge flags */
#define VMA_MERGE_PREV 0x1
#define VMA_MERGE_NEXT 0x2

/* Fault flags */
#define FAULT_FLAG_WRITE 0x01
#define FAULT_FLAG_USER  0x02
#define FAULT_FLAG_INSTR 0x04
#define FAULT_FLAG_SPECULATIVE 0x08

/* Standard fault return codes */
#define VM_FAULT_OOM    0x0001
#define VM_FAULT_SIGBUS 0x0002
#define VM_FAULT_SIGSEGV 0x0004
#define VM_FAULT_MAJOR  0x0008
#define VM_FAULT_RETRY  0x0010
#define VM_FAULT_COMPLETED 0x0020

/* MM Operations */
void mm_init(struct mm_struct *mm);
void mm_destroy(struct mm_struct *mm);
struct mm_struct *mm_alloc(void);
struct mm_struct *mm_create(void);
struct mm_struct *mm_copy(struct mm_struct *old_mm);
void mm_free(struct mm_struct *mm);
void mm_get(struct mm_struct *mm);
void mm_put(struct mm_struct *mm);

/* VMA Operations */
struct vm_area_struct *vma_alloc(void);
void vma_free(struct vm_area_struct *vma);
struct vm_area_struct *vma_create(uint64_t start, uint64_t end, uint64_t flags);

struct file;

/* High-level VMA management (POSIX-like) */
uint64_t do_mmap(struct mm_struct *mm, uint64_t addr, size_t len, uint64_t prot, uint64_t flags, struct file *file, uint64_t pgoff);
int do_munmap(struct mm_struct *mm, uint64_t addr, size_t len);
int do_mprotect(struct mm_struct *mm, uint64_t addr, size_t len, uint64_t prot);

/* Internal VMA Helpers */
struct vm_area_struct *vma_find(struct mm_struct *mm, uint64_t addr);
struct vm_area_struct *vma_find_exact(struct mm_struct *mm, uint64_t start, uint64_t end);
struct vm_area_struct *vma_find_intersection(struct mm_struct *mm, uint64_t start, uint64_t end);

int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma);
void vma_remove(struct mm_struct *mm, struct vm_area_struct *vma);
int vma_split(struct mm_struct *mm, struct vm_area_struct *vma, uint64_t addr);
struct vm_area_struct *vma_merge(struct mm_struct *mm, struct vm_area_struct *prev,
                                 uint64_t addr, uint64_t end, uint64_t vm_flags,
                                 struct vm_object *obj, uint64_t pgoff);

/* VMA Allocation Search */
uint64_t vma_find_free_region(struct mm_struct *mm, size_t size, uint64_t range_start, uint64_t range_end);
uint64_t vma_find_free_region_aligned(struct mm_struct *mm, size_t size, uint64_t alignment, uint64_t range_start, uint64_t range_end);

/* High-level VMA operations */
int vma_map_range(struct mm_struct *mm, uint64_t start, uint64_t end,
                  uint64_t flags);
int vma_unmap_range(struct mm_struct *mm, uint64_t start, uint64_t end);
int vma_protect(struct mm_struct *mm, uint64_t start, uint64_t end,
                uint64_t new_flags);
int mm_populate_user_range(struct mm_struct *mm, uint64_t start, size_t size, uint64_t flags, const uint8_t *data, size_t data_len);

/* Page fault dispatch */
int handle_mm_fault(struct vm_area_struct *vma, uint64_t address, unsigned int flags);

/* Accounting */
void mm_update_accounting(struct mm_struct *mm);
size_t mm_total_size(struct mm_struct *mm);
size_t mm_map_count(struct mm_struct *mm);

/* VMA Iteration */
struct vm_area_struct *vma_next(struct vm_area_struct *vma);
struct vm_area_struct *vma_prev(struct vm_area_struct *vma);

/* VMA Locking */
static inline void vma_lock(struct vm_area_struct *vma) {
    down_write(&vma->vm_lock);
}

static inline void vma_unlock(struct vm_area_struct *vma) {
    up_write(&vma->vm_lock);
}

static inline void vma_lock_shared(struct vm_area_struct *vma) {
    down_read(&vma->vm_lock);
}

static inline void vma_unlock_shared(struct vm_area_struct *vma) {
    up_read(&vma->vm_lock);
}

static inline int vma_trylock(struct vm_area_struct *vma) {
    return down_write_trylock(&vma->vm_lock);
}

static inline int vma_trylock_shared(struct vm_area_struct *vma) {
    return down_read_trylock(&vma->vm_lock);
}

/* Statistics and Debugging */
void vma_dump(struct mm_struct *mm);
void vma_dump_single(struct vm_area_struct *vma);
size_t mm_total_size(struct mm_struct *mm);
size_t mm_map_count(struct mm_struct *mm);

/* Generic fault handler */
int handle_mm_fault(struct vm_area_struct *vma, uint64_t address, unsigned int flags);

/* RMAP Helpers */
void anon_vma_free(struct anon_vma *av);
int anon_vma_prepare(struct vm_area_struct *vma);
int anon_vma_chain_link(struct vm_area_struct *vma, struct anon_vma *av);

/* Reclamation */
void lru_init(void);
void kswapd_init(void);
void khugepaged_init(void);

struct folio;
struct vm_object;

void folio_add_anon_rmap(struct folio *folio, struct vm_area_struct *vma, uint64_t address);
void folio_add_file_rmap(struct folio *folio, struct vm_object *obj, uint64_t pgoff);

struct mmu_gather;
int try_to_unmap_folio(struct folio *folio, struct mmu_gather *tlb);
int folio_referenced(struct folio *folio);
int folio_reclaim(struct folio *folio, struct mmu_gather *tlb);

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

/* MM Scrubber */
void mm_scrubber_init(void);

/* Helper macros */
#define vma_pages(vma) (((vma)->vm_end - (vma)->vm_start) >> PAGE_SHIFT)
#define vma_size(vma) ((vma)->vm_end - (vma)->vm_start)

#define for_each_vma(mm, vma)                                                  \
  for (unsigned long __idx = 0;                                                \
       (vma = mt_find(&(mm)->mm_mt, &__idx, ULONG_MAX)) != NULL; )

#define for_each_vma_safe(mm, vma, tmp)                                        \
  for (unsigned long __idx = 0;                                                \
       (vma = mt_find(&(mm)->mm_mt, &__idx, ULONG_MAX)) != NULL &&              \
       ({ tmp = vma; 1; }); )

#define for_each_vma_range(mm, vma, start, end)                                \
  for (unsigned long __idx = (start);                                          \
       (vma = mt_find(&(mm)->mm_mt, &__idx, (end) - 1)) != NULL; )

#define for_each_vma_range_safe(mm, vma, tmp, start, end)                      \
  for (unsigned long __idx = (start);                                          \
       (vma = mt_find(&(mm)->mm_mt, &__idx, (end) - 1)) != NULL &&              \
       ({ tmp = vma; 1; }); )