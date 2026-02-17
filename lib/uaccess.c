#include <lib/uaccess.h>
#include <aerosync/export.h>

/* These are implemented in arch/x86_64/lib/uaccess.asm */
extern size_t __copy_from_user(void *to, const void __user *from, size_t n);
extern size_t __copy_to_user(void __user *to, const void *from, size_t n);

size_t copy_from_user(void *to, const void __user *from, size_t n) {
    if (!access_ok(from, n))
        return n;

    return __copy_from_user(to, from, n);
}
EXPORT_SYMBOL(copy_from_user);

size_t copy_to_user(void __user *to, const void *from, size_t n) {
    if (!access_ok(to, n))
        return n;

    return __copy_to_user(to, from, n);
}
EXPORT_SYMBOL(copy_to_user);