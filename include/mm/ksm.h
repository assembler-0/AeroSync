#pragma once

#include <mm/mm_types.h>

#ifdef CONFIG_MM_KSM
int ksm_init(void);
int ksm_madvise(struct vm_area_struct *vma, uint64_t start, uint64_t end,
                int advice);
int ksm_enter(struct mm_struct *mm);
#else
static inline int ksm_init(void) { return 0; }
static inline int ksm_madvise(struct vm_area_struct *vma, uint64_t start,
                              uint64_t end, int advice) {
  return 0;
}
static inline int ksm_enter(struct mm_struct *mm) { return 0; }
#endif
