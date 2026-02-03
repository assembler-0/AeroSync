#include <mm/shm.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <aerosync/spinlock.h>
#include <aerosync/errno.h>
#include <linux/container_of.h>

/* Global registry of shared memory objects */
static LIST_HEAD(shm_list);
static DEFINE_SPINLOCK(shm_lock);

int shm_init(void) {
    spinlock_init(&shm_lock);
    printk(KERN_INFO SHM_CLASS "SHM manager initialized.\n");
    return 0;
}

static struct shm_object *find_shm_locked(const char *name) {
    struct shm_object *shm;
    list_for_each_entry(shm, &shm_list, list) {
        if (strncmp(shm->name, name, SHM_NAME_MAX) == 0) {
            return shm;
        }
    }
    return nullptr;
}

struct shm_object *shm_open(const char *name, size_t size, int flags) {
    struct shm_object *shm;

    spinlock_lock(&shm_lock);
    shm = find_shm_locked(name);

    if (shm) {
        /* Object exists, just take a reference */
        atomic_inc(&shm->refcount);
        spinlock_unlock(&shm_lock);
        return shm;
    }

    /* Create new object */
    shm = kmalloc(sizeof(struct shm_object));
    if (!shm) {
        spinlock_unlock(&shm_lock);
        return nullptr;
    }

    strncpy(shm->name, name, SHM_NAME_MAX);
    atomic_set(&shm->refcount, 1);
    
    /* Create the backing anonymous object */
    shm->vmo = vm_object_anon_create(PAGE_ALIGN_UP(size));
    if (!shm->vmo) {
        kfree(shm);
        spinlock_unlock(&shm_lock);
        return nullptr;
    }

    list_add(&shm->list, &shm_list);
    spinlock_unlock(&shm_lock);

    return shm;
}

void shm_close(struct shm_object *shm) {
    if (!shm) return;

    /* 
     * In this named model, shm_close only drops the 'handle' reference.
     * The global registry keeps a reference until shm_unlink.
     */
    if (atomic_dec_and_test(&shm->refcount)) {
        /* Only destroy if it was already unlinked and this was the last ref */
        vm_object_put(shm->vmo);
        kfree(shm);
    }
}

int shm_unlink(const char *name) {
    struct shm_object *shm;

    spinlock_lock(&shm_lock);
    shm = find_shm_locked(name);

    if (!shm) {
        spinlock_unlock(&shm_lock);
        return -ENOENT;
    }

    list_del_init(&shm->list);
    spinlock_unlock(&shm_lock);

    /* Registry drops its reference */
    shm_close(shm);
    return 0;
}
