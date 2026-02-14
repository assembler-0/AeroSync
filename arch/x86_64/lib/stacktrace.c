#include <aerosync/stacktrace.h>
#include <aerosync/ksymtab.h>
#include <aerosync/export.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <arch/x86_64/mm/vmm.h>

// Helper to check if address is likely a valid kernel stack address
static int is_valid_stack_addr(uintptr_t addr) {
  if (addr < vmm_get_canonical_high_base()) return 0; // Not in high half
  if (addr & 0x7) return 0; // Not aligned
  return 1;
}

static void print_stack_hexdump(uintptr_t stack_ptr, int count) {
  printk(KERN_EMERG STACKTRACE_CLASS "Stack Dump: \n");
  uint64_t *ptr = (uint64_t *)stack_ptr;
  for (int i = 0; i < count; i++) {
    if (!is_valid_stack_addr((uintptr_t)&ptr[i])) break;
    printk(KERN_EMERG STACKTRACE_CLASS "  %016llx\n", ptr[i]);
  }
}

void dump_stack_from(uint64_t rbp, uint64_t rip) {
  uintptr_t *frame = (uintptr_t *) rbp;
  int depth = 0;

  printk(KERN_EMERG STACKTRACE_CLASS "Call Trace (RBP: %016llx, RIP: %016llx):\n", rbp, rip);

  // Print the starting RIP if provided
  if (rip) {
    uintptr_t offset = 0;
    const char *name = lookup_ksymbol_by_addr(rip, &offset);
    if (name) {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016llx>] %s+0x%lx\n", rip, name, offset);
    } else {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016llx>] ?\n", rip);
    }
  }

  while (depth < 32) {
    if (!is_valid_stack_addr((uintptr_t)frame)) {
      if (frame) printk(KERN_EMERG STACKTRACE_CLASS "  <Invalid Frame Pointer: %016llx>\n", (uintptr_t)frame);
      break;
    }

    // In x86_64 with frame pointers:
    // [rbp] = previous rbp
    // [rbp + 8] = return address

    uintptr_t ret_addr = frame[1];
    if (!ret_addr) break;

    uintptr_t offset = 0;
    const char *name = lookup_ksymbol_by_addr(ret_addr, &offset);

    if (name) {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016lx>] %s+0x%lx\n", ret_addr, name, offset);
    } else {
      printk(KERN_EMERG STACKTRACE_CLASS "  [<%016lx>] ?\n", ret_addr);
    }

    // Dump a small window of stack memory (args/locals) just above the return address
    // frame + 2 starts the data pushed before the call
    print_stack_hexdump((uintptr_t)(frame + 2), 4); // Dump 4 qwords (32 bytes)

    uintptr_t next_rbp = frame[0];

    // Basic sanity checks to avoid infinite loops or bad dereferences
    if (next_rbp <= (uintptr_t) frame) break;
    
    frame = (uintptr_t *) next_rbp;
    depth++;
  }
}
EXPORT_SYMBOL(dump_stack_from);

void __no_cfi dump_stack(void) {
  // Get the frame pointer of the current function (dump_stack)
  uintptr_t rbp = (uintptr_t)__builtin_frame_address(0);

  if (!is_valid_stack_addr(rbp)) {
    printk(KERN_EMERG STACKTRACE_CLASS "dump_stack: invalid RBP %016llx\n", rbp);
    return;
  }

  // In x86_64 with frame pointers:
  // [rbp] = caller's RBP
  // [rbp + 8] = caller's RIP (return address)
  uintptr_t caller_rbp = *(uintptr_t*)rbp;
  uintptr_t caller_rip = *(uintptr_t*)(rbp + 8);

  dump_stack_from(caller_rbp, caller_rip);
}
EXPORT_SYMBOL(dump_stack);


