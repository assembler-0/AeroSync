#include <lib/uaccess.h>
#include <lib/string.h>
#include <arch/x64/mm/vmm.h>

size_t copy_from_user(void *to, const void *from, size_t n) {
    if (!access_ok(from, n))
        return n;

    // TODO: In a production kernel, we would use a specialized ASM routine
    // with exception tables to catch page faults. For now, we rely on
    // the page fault handler to fix things up or panic if it's a kernel fault
    // that happened on behalf of a user.
    
    memcpy(to, from, n);
    return 0;
}

size_t copy_to_user(void *to, const void *from, size_t n) {
    if (!access_ok(to, n))
        return n;

    memcpy(to, from, n);
    return 0;
}
