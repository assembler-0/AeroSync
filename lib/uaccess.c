#include <lib/uaccess.h>
#include <arch/x86_64/mm/vmm.h>

/* These are implemented in arch/x86_64/lib/uaccess.asm */
extern size_t __copy_from_user(void *to, const void *from, size_t n);
extern size_t __copy_to_user(void *to, const void *from, size_t n);

size_t copy_from_user(void *to, const void *from, size_t n) {
    if (!access_ok(from, n))
        return n;

    return __copy_from_user(to, from, n);
}

size_t copy_to_user(void *to, const void *from, size_t n) {
    if (!access_ok(to, n))
        return n;

    return __copy_to_user(to, from, n);
}