#include <aerosync/stacktrace.h>
#include <aerosync/ksymtab.h>
#include <aerosync/export.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/cpu.h>
#include <aerosync/sched/sched.h>

/* ========================================================================
 * Conservative Stack Unwinder
 * ======================================================================= */

/**
 * is_ptr_accessible - Extremely conservative memory validation
 * @ptr: Address to check
 * 
 * Checks if a pointer is likely safe to dereference in a panic context.
 * In AeroSync, we assume kernel code/data and stacks are in the higher half.
 */
static int is_ptr_accessible(uintptr_t ptr) {
  /* 1. Canonical high half check */
  if (ptr < vmm_get_canonical_high_base()) return 0;
  
  /* 2. Alignment check (x86_64 stack frames must be 8-byte aligned) */
  if (ptr & 0x7) return 0;

  /* 
   * 3. Stack range validation
   * We try to find the current CPU's stack boundaries if available.
   */
  struct task_struct *curr = get_current();
  if (curr && curr->stack) {
    uintptr_t stack_base = (uintptr_t)curr->stack;
    uintptr_t stack_top = stack_base + PAGE_SIZE * 4; // TODO: make this a global
    if (ptr >= stack_base && ptr < stack_top) return 1;
  }

  /*
   * 4. Fallback: If not in task stack, check if it's in the per-CPU TSS stacks 
   * (IST stacks for double faults, NMI, etc.)
   */
  // TODO: Implement IST range check if needed. For now, canonical high half is minimum.
  
  return 1; 
}

static void print_stack_hexdump_safe(uintptr_t stack_ptr, int count) {
  uint64_t *ptr = (uint64_t *)stack_ptr;
  
  for (int i = 0; i < count; i++) {
    uintptr_t addr = (uintptr_t)&ptr[i];
    if (!is_ptr_accessible(addr)) break;
    
    /* We don't want a fault here, so we just print the raw hex */
    printk(KERN_EMERG STACKTRACE_CLASS "    +%03x: %016llx\n", i * 8, ptr[i]);
  }
}

void dump_stack_from(uint64_t rbp, uint64_t rip) {
  uintptr_t *frame = (uintptr_t *) rbp;
  int depth = 0;

  printk(KERN_EMERG STACKTRACE_CLASS "Call Trace (Unwinding from RBP: %016llx):\n", rbp);

  /* 1. Validate starting RIP */
  if (rip) {
    uintptr_t offset = 0;
    const char *name = lookup_ksymbol_by_addr(rip, &offset);
    if (name) {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016llx>] %s+0x%lx\n", rip, name, offset);
    } else {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016llx>] <Unknown Symbol>\n", rip);
    }
  }

  /* 2. Unwind Loop */
  while (depth < 24) { /* Conservative limit to prevent infinite loops */
    if (!is_ptr_accessible((uintptr_t)frame)) {
      if (frame) printk(KERN_EMERG STACKTRACE_CLASS "  <Inaccessible/Unmapped Frame: %016llx>\n", (uintptr_t)frame);
      break;
    }

    /* frame[0] = next RBP (saved RBP)
     * frame[1] = return RIP 
     */
    
    /* Validate that we can read at least 2 qwords from this frame */
    if (!is_ptr_accessible((uintptr_t)&frame[1])) break;

    uintptr_t ret_addr = frame[1];
    if (!ret_addr) break;

    /* Check if return address is in kernel space */
    if (ret_addr < vmm_get_canonical_high_base()) {
        printk(KERN_EMERG STACKTRACE_CLASS "  <Return address in userspace: %016llx>\n", ret_addr);
        break;
    }

    uintptr_t offset = 0;
    const char *name = lookup_ksymbol_by_addr(ret_addr, &offset);

    if (name) {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016lx>] %s+0x%lx\n", ret_addr, name, offset);
    } else {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016lx>] ?\n", ret_addr);
    }

    /* Optional: Small safe hexdump of the stack frame */
    // print_stack_hexdump_safe((uintptr_t)(frame + 2), 2); 

    uintptr_t next_rbp = frame[0];

    /* Sanity checks to prevent loops or jumping backwards in memory */
    if (next_rbp <= (uintptr_t) frame) {
        if (next_rbp != 0)
            printk(KERN_EMERG STACKTRACE_CLASS "  <Stack Unwind Corrupted: next_rbp <= current>\n");
        break;
    }
    
    /* Ensure the step isn't suspiciously large (e.g. > 1 page per frame) */
    if (next_rbp - (uintptr_t)frame > 4096) {
        printk(KERN_EMERG STACKTRACE_CLASS "  <Suspiciously large stack frame: 0x%lx>\n", next_rbp - (uintptr_t)frame);
        break;
    }

    frame = (uintptr_t *) next_rbp;
    depth++;
  }
  
  if (depth == 24) {
      printk(KERN_EMERG STACKTRACE_CLASS "  <Unwind limit reached>\n");
  }
}
EXPORT_SYMBOL(dump_stack_from);

void __no_cfi dump_stack(void) {
  /* Use compiler builtin to get frame pointer safely */
  uintptr_t rbp = (uintptr_t)__builtin_frame_address(0);

  if (!is_ptr_accessible(rbp)) {
    printk(KERN_EMERG STACKTRACE_CLASS "dump_stack: current RBP %016llx is invalid\n", rbp);
    return;
  }

  /* 
   * In x86_64 with frame pointers:
   * [rbp] = caller's RBP
   * [rbp + 8] = caller's RIP (return address)
   */
  uintptr_t *ptr = (uintptr_t *)rbp;
  
  /* Validate caller's frame can be read */
  if (!is_ptr_accessible((uintptr_t)ptr) || !is_ptr_accessible((uintptr_t)&ptr[1])) {
      printk(KERN_EMERG STACKTRACE_CLASS "dump_stack: cannot read caller frame\n");
      return;
  }

  uintptr_t caller_rbp = ptr[0];
  uintptr_t caller_rip = ptr[1];

  dump_stack_from(caller_rbp, caller_rip);
}
EXPORT_SYMBOL(dump_stack);
