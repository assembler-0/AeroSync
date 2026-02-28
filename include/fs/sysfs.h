/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/fs/sysfs.h
 * @brief sysfs public interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <fs/vfs.h>

struct device;
struct class;
struct bus_type;

void sysfs_init(void);

int sysfs_register_device(struct device *dev);
void sysfs_unregister_device(struct device *dev);

int sysfs_register_class(struct class *cls);
void sysfs_unregister_class(struct class *cls);

int sysfs_register_bus(struct bus_type *bus);
void sysfs_unregister_bus(struct bus_type *bus);

/* Helpers for kernel systems */
int sysfs_create_dir_kern(const char *name, const char *parent);
int sysfs_create_file_kern(const char *name, const struct file_operations *fops, void *private_data, const char *parent);
