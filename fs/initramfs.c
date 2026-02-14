/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/initramfs.c
 * @brief Initramfs (CPIO) unpacking implementation
 * @copyright (C) 2026 assembler-0
 */

#include <fs/initramfs.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <aerosync/errno.h>
#include <aerosync/limine_modules.h>
#include <mm/slub.h>
#include <aerosync/classes.h>
#include <fs/file.h>

static inline uint32_t parse_hex(const char *s, size_t len) {
  char tmp[9];
  if (len > 8) len = 8;
  memcpy(tmp, s, len);
  tmp[len] = '\0';
  return (uint32_t)simple_strtoul(tmp, nullptr, 16);
}

static inline size_t cpio_align(size_t n) {
  return (n + 3) & ~3;
}

int initramfs_unpack(void *data, size_t size) {
  uint8_t *p = (uint8_t *)data;
  uint8_t *end = p + size;

  if (!data || size < 6) {
    return -EINVAL;
  }

  /* Skip leading zeros if any (some bootloaders might pad) */
  while (p + 6 <= end && *p == 0) p++;

  if (p + 6 > end) {
     return -EINVAL;
  }

  while (p + sizeof(struct cpio_newc_header) <= end) {
    struct cpio_newc_header *hdr = (struct cpio_newc_header *)p;

    if (memcmp(hdr->c_magic, CPIO_NEWC_MAGIC, 6) != 0) {
      break;
    }

    uint32_t namesize = parse_hex(hdr->c_namesize, 8);
    uint32_t filesize = parse_hex(hdr->c_filesize, 8);
    uint32_t mode = parse_hex(hdr->c_mode, 8);

    /* CPIO newc format alignment:
     * - The header is 110 bytes.
     * - The name follows the header and is padded with null bytes to a multiple of 4 bytes (header + name).
     * - The file data follows the padded name and is itself padded to a multiple of 4 bytes.
     */

    char *name = (char *)(p + sizeof(struct cpio_newc_header));
    if ((uint8_t *)name + namesize > end) {
      return -EINVAL;
    }

    if (namesize == 0) {
      p += cpio_align(sizeof(struct cpio_newc_header));
      continue;
    }

    /* Use a temporary buffer for name to ensure it's null-terminated if it isn't in CPIO */
    char name_buf[256];
    size_t name_len = namesize;
    if (name_len > 255) name_len = 255;
    memcpy(name_buf, name, name_len);
    /* CPIO names include a null terminator in namesize, but let's be safe */
    if (name_len > 0) name_buf[name_len - 1] = '\0';
    else name_buf[0] = '\0';
    
    char *clean_name = name_buf;
    if (clean_name[0] == '.' && clean_name[1] == '/') {
      clean_name += 2;
    }

    if (strcmp(clean_name, "TRAILER!!!") == 0) {
      break;
    }

    if (clean_name[0] == '\0' || strcmp(clean_name, ".") == 0) {
      p += cpio_align(sizeof(struct cpio_newc_header) + namesize);
      p += cpio_align(filesize);
      continue;
    }

    if (strcmp(clean_name, "..") == 0) {
      p += cpio_align(sizeof(struct cpio_newc_header) + namesize);
      p += cpio_align(filesize);
      continue;
    }

    /* Create the full path (ensuring it starts with /) */
    char full_path[1024];
    if (clean_name[0] == '/') {
      strncpy(full_path, clean_name, sizeof(full_path) - 1);
      full_path[sizeof(full_path) - 1] = '\0';
    } else {
      full_path[0] = '/';
      strncpy(full_path + 1, clean_name, sizeof(full_path) - 2);
      full_path[sizeof(full_path) - 1] = '\0';
    }
    
    uint8_t *file_data = (uint8_t *)p + cpio_align(sizeof(struct cpio_newc_header) + namesize);
    if (file_data + filesize > end) {
      return -EINVAL;
    }

    if (S_ISDIR(mode)) {
      /* Ensure parent directories exist */
      char *p_copy = kstrdup(full_path);
      if (p_copy) {
        char *s = strchr(p_copy + 1, '/');
        while (s) {
          *s = '\0';
          do_mkdir(p_copy, 0755);
          *s = '/';
          s = strchr(s + 1, '/');
        }
        do_mkdir(p_copy, mode & 0777);
        kfree(p_copy);
      }
    } else if (S_ISREG(mode)) {
      /* Ensure parent directories exist */
      char *p_copy = kstrdup(full_path);
      if (p_copy) {
        char *slash = strrchr(p_copy, '/');
        if (slash && slash != p_copy) {
          *slash = '\0';
          char *s = strchr(p_copy + 1, '/');
          while (s) {
            *s = '\0';
            do_mkdir(p_copy, 0755);
            *s = '/';
            s = strchr(s + 1, '/');
          }
          do_mkdir(p_copy, 0755);
        }
        kfree(p_copy);
      }

      struct file *f = vfs_open(full_path, O_CREAT | O_WRONLY | O_TRUNC, mode & 0777);
      if (f) {
        vfs_loff_t pos = 0;
        kernel_write(f, (const char *)file_data, filesize, &pos);
        vfs_close(f);
      }
    } else if (S_ISLNK(mode)) {
      char *target = kmalloc(filesize + 1);
      if (target) {
        memcpy(target, file_data, filesize);
        target[filesize] = '\0';
        do_symlink(target, full_path);
        kfree(target);
      }
    }

    p += cpio_align(sizeof(struct cpio_newc_header) + namesize);
    p += cpio_align(filesize);
  }

  printk(KERN_INFO INITRD_CLASS "Unpacking complete.\n");
  return 0;
}

int initramfs_cpio_prober(const struct limine_file *file, lmm_type_t *out_type) {
  if (file->size < 6) return 0;
  if (memcmp((void *)file->address, CPIO_NEWC_MAGIC, 6) == 0) {
    *out_type = LMM_TYPE_INITRD;
    return 80;
  }
  return 0;
}

void initramfs_init(const char *initrd_name) {
#ifdef CONFIG_INITRAMFS
  struct lmm_entry *entry = nullptr;

  /* Try to find by name if provided */
  if (initrd_name && initrd_name[0] != '\0') {
    entry = lmm_find_module(initrd_name);
    if (!entry) {
      printk(KERN_WARNING INITRD_CLASS "Initramfs '%s' specified but not found.\n", initrd_name);
    }
  }

  /* Fallback: Try to find by type (auto-detection) */
  if (!entry) {
    entry = lmm_find_module_by_type(LMM_TYPE_INITRD);
    if (entry) {
      printk(KERN_INFO INITRD_CLASS "Auto-detected initramfs module: %s\n", entry->file->path);
    }
  }

  if (entry) {
    initramfs_unpack((void *)entry->file->address, entry->file->size);
  } else {
    if (initrd_name && initrd_name[0] != '\0') {
      /* Only warn if user specifically asked for one or if we really expected one */
       printk(KERN_NOTICE INITRD_CLASS "No initramfs module found.\n");
    } else {
      /* Silent if nothing was asked and nothing found (maybe intentional) */
      printk(KERN_DEBUG INITRD_CLASS "No initramfs module found (none specified, none detected).\n");
    }
  }
#endif
}