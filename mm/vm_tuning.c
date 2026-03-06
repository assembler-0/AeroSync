/// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file mm/vm_tuning.c
 * @brief Runtime VM tuning parameters
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/vm_tuning.h>
#include <aerosync/atomic.h>

struct vm_tuning_params vm_tuning = {
    .eager_copy_threshold = ATOMIC_INIT(CONFIG_MM_SHADOW_DEPTH_LIMIT),
    .tlb_batch_size = ATOMIC_INIT(CONFIG_MM_TLB_BATCH_SIZE),
    .collapse_threshold = ATOMIC_INIT(CONFIG_MM_SHADOW_DEPTH_LIMIT),
    .shadow_max_depth = ATOMIC_INIT(CONFIG_MM_SHADOW_DEPTH_LIMIT),
};
