#include <fs/vfs.h>
#include <fs/file.h>
#include <mm/slub.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <lib/string.h>
#include <lib/bitmap.h>
#include <aerosync/panic.h>

struct files_struct init_files = {
    .count = ATOMIC_INIT(1),
    .file_lock = SPINLOCK_INIT,
    .next_fd = 0,
    .fdtab = {
        .max_fds = NR_OPEN_DEFAULT,
    }
};

// We need a way to initialize the pointers that depend on the struct address
void files_init(void) {
    init_files.fdtab.fd = init_files.fd_array;
    init_files.fdtab.open_fds = init_files.open_fds_init;
    init_files.fdtab.close_on_exec = init_files.close_on_exec_init;
}

struct file *fget(unsigned int fd) {
    struct files_struct *files = current->files;
    struct file *file = NULL;

    if (!files) return NULL;

    spinlock_lock(&files->file_lock);
    if (fd < files->fdtab.max_fds) {
        file = files->fdtab.fd[fd];
        if (file) {
            atomic_inc(&file->f_count);
        }
    }
    spinlock_unlock(&files->file_lock);

    return file;
}

void fput(struct file *file) {
    if (!file) return;

    if (atomic_dec_and_test(&file->f_count)) {
        vfs_close(file);
    }
}

int fd_install(unsigned int fd, struct file *file) {
    struct files_struct *files = current->files;
    if (!files) return -1;

    spinlock_lock(&files->file_lock);
    if (fd >= files->fdtab.max_fds) {
        spinlock_unlock(&files->file_lock);
        return -1;
    }
    files->fdtab.fd[fd] = file;
    // Initial refcount is already set when file is created (vfs_open)
    spinlock_unlock(&files->file_lock);
    return 0;
}

int get_unused_fd_flags(unsigned int flags) {
    struct files_struct *files = current->files;
    if (!files) return -1;

    spinlock_lock(&files->file_lock);
    int fd = find_next_zero_bit(files->fdtab.open_fds, files->fdtab.max_fds, files->next_fd);
    
    if (fd >= files->fdtab.max_fds) {
        // Here we would normally expand the fd table
        spinlock_unlock(&files->file_lock);
        return -1;
    }

    set_bit(fd, files->fdtab.open_fds);
    if (flags & O_CLOEXEC) {
        set_bit(fd, files->fdtab.close_on_exec);
    } else {
        clear_bit(fd, files->fdtab.close_on_exec);
    }

    files->next_fd = fd + 1;
    spinlock_unlock(&files->file_lock);
    return fd;
}

void put_unused_fd(unsigned int fd) {
    struct files_struct *files = current->files;
    if (!files) return;

    spinlock_lock(&files->file_lock);
    if (fd < files->fdtab.max_fds) {
        clear_bit(fd, files->fdtab.open_fds);
        if (fd < files->next_fd) {
            files->next_fd = fd;
        }
    }
    spinlock_unlock(&files->file_lock);
}

// Helper to allocate and initialize a files_struct (for fork)
struct files_struct *copy_files(struct files_struct *old_files) {
    struct files_struct *new_files = kzalloc(sizeof(struct files_struct));
    if (!new_files) return NULL;

    atomic_set(&new_files->count, 1);
    spinlock_init(&new_files->file_lock);
    new_files->next_fd = 0;
    new_files->fdtab.max_fds = NR_OPEN_DEFAULT;
    new_files->fdtab.fd = new_files->fd_array;
    new_files->fdtab.open_fds = new_files->open_fds_init;
    new_files->fdtab.close_on_exec = new_files->close_on_exec_init;

    if (old_files) {
        spinlock_lock(&old_files->file_lock);
        for (int i = 0; i < old_files->fdtab.max_fds; i++) {
            if (test_bit(i, old_files->fdtab.open_fds)) {
                struct file *f = old_files->fdtab.fd[i];
                new_files->fd_array[i] = f;
                set_bit(i, new_files->open_fds_init);
                if (test_bit(i, old_files->fdtab.close_on_exec))
                    set_bit(i, new_files->close_on_exec_init);
                
                if (f) atomic_inc(&f->f_count);
            }
        }
        new_files->next_fd = old_files->next_fd;
        spinlock_unlock(&old_files->file_lock);
    }

    return new_files;
}
