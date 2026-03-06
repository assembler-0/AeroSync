#pragma once

#include <aerosync/atomic.h>

/**
 * Runtime VM tuning parameters
 * Accessible via /runtime/sys/mm/vm/
 */
struct vm_tuning_params {
    atomic_t eager_copy_threshold;  /* Shadow depth before eager copy (default: 4) */
    atomic_t tlb_batch_size;        /* TLB batch threshold (default: 32) */
    atomic_t collapse_threshold;    /* Auto-collapse depth (default: 4) */
    atomic_t shadow_max_depth;      /* Hard limit on shadow depth (default: 8) */
};

extern struct vm_tuning_params vm_tuning;

/* Accessor macros for performance */
#define VM_EAGER_COPY_THRESHOLD() atomic_read(&vm_tuning.eager_copy_threshold)
#define VM_TLB_BATCH_SIZE() atomic_read(&vm_tuning.tlb_batch_size)
#define VM_COLLAPSE_THRESHOLD() atomic_read(&vm_tuning.collapse_threshold)
#define VM_SHADOW_MAX_DEPTH() atomic_read(&vm_tuning.shadow_max_depth)
