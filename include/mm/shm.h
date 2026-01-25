#pragma once

#include <mm/vm_object.h>

/**
 * AeroSync Shared Memory Manager
 * 
 * Provides named, reference-counted memory objects that can be 
 * mapped into multiple address spaces.
 */

#define SHM_NAME_MAX 64

struct shm_object {
    char name[SHM_NAME_MAX];
    struct vm_object *vmo;
    atomic_t refcount;
    struct list_head list; /* Node in global shm_list */
};

/* API */
int shm_init(void);

/**
 * shm_open - Creates or opens a named shared memory object.
 * Returns a pointer to the shm_object (refcounted).
 */
struct shm_object *shm_open(const char *name, size_t size, int flags);

/**
 * shm_close - Releases a reference to an shm_object.
 */
void shm_close(struct shm_object *shm);

/**
 * shm_unlink - Removes the name from the global registry.
 * The object will be destroyed when the last reference is closed.
 */
int shm_unlink(const char *name);
