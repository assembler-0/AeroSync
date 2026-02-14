#pragma once

#include <fs/vfs.h>

void devfs_init(void);
int devfs_register_device(const char *name, vfs_mode_t mode, dev_t dev, 
                          const struct file_operations *fops, void *private_data);
