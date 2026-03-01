/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/initramfs.c
 * @brief Initramfs (CPIO) unpacking implementation - Bulletproof Edition
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
#include <arch/x86_64/requests.h>

#define CPIO_SEARCH_WINDOW 4096

/* Manual hex parser to avoid dependency on potentially broken kstrtox/simple_strtoul */
static uint32_t hex8_to_u32(const char *s) {
  uint32_t res = 0;
  for (int i = 0; i < 8; i++) {
    char c = s[i];
    uint32_t val = 0;
    if (c >= '0' && c <= '9') val = c - '0';
    else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
    else break;
    res = (res << 4) | val;
  }
  return res;
}

static inline size_t cpio_align(size_t n) {
  return (n + 3) & ~3;
}

int initramfs_unpack(void *data, size_t size) {
  uint8_t *p = (uint8_t *) data;
  uint8_t *end = p + size;

  printk(KERN_INFO INITRD_CLASS "Unpacking initramfs (address=%p, size=%zu)\n", data, size);

  if (!data || size < 6) {
    return -EINVAL;
  }

  int files_created = 0;
  int dirs_created = 0;
  int records_found = 0;
  bool debug = cmdline_find_option_bool(current_cmdline, "initrd_debug");

  while (p + 110 <= end) {
    /* Search for magic */
    uint8_t *search_ptr = p;
    uint8_t *search_end = end - 6;
    bool magic_found = false;

    while (search_ptr < search_end) {
      if (memcmp(search_ptr, CPIO_NEWC_MAGIC, 6) == 0) {
        magic_found = true;
        break;
      }
      search_ptr++;
    }

    if (!magic_found) break;

    p = search_ptr;
    records_found++;

    /* CPIO newc offsets: 
     * magic: 0, ino: 6, mode: 14, uid: 22, gid: 30, nlink: 38, mtime: 46, 
     * filesize: 54, devmaj: 62, devmin: 70, rdevmaj: 78, rdevmin: 86, 
     * namesize: 94, check: 102 
     */
    uint32_t mode = hex8_to_u32((const char *) (p + 14));
    uint32_t filesize = hex8_to_u32((const char *) (p + 54));
    uint32_t namesize = hex8_to_u32((const char *) (p + 94));

    if (namesize == 0 || namesize > 1024) {
      p += 6; /* Skip magic and continue search */
      continue;
    }

    char name_buf[256];
    size_t name_len = namesize;
    if (name_len > 255) name_len = 255;
    memcpy(name_buf, p + 110, name_len);
    name_buf[name_len - 1] = '\0';

    if (debug) {
      printk(KERN_INFO INITRD_CLASS "Record [%d]: '%s' (filesize=%u, mode=%06o)\n",
             records_found, name_buf, filesize, mode);
    }

    if (strcmp(name_buf, "TRAILER!!!") == 0) {
      break;
    }

    char *clean_name = name_buf;
    if (clean_name[0] == '.' && clean_name[1] == '/') {
      clean_name += 2;
    }
    if (strncmp(clean_name, "initrd/", 7) == 0) {
      clean_name += 7;
    }

    if (clean_name[0] == '\0' || strcmp(clean_name, ".") == 0 || strcmp(clean_name, "..") == 0) {
      p += cpio_align(110 + namesize);
      p += cpio_align(filesize);
      continue;
    }

    char full_path[1024];
    full_path[0] = '/';
    strncpy(full_path + 1, clean_name, sizeof(full_path) - 2);
    full_path[sizeof(full_path) - 1] = '\0';

    uint8_t *file_data = p + cpio_align(110 + namesize);
    if (file_data + filesize > end) {
      printk(KERN_ERR INITRD_CLASS "Data for %s exceeds buffer\n", full_path);
      break;
    }

    if (S_ISDIR(mode)) {
      char *p_copy = kstrdup(full_path);
      if (p_copy) {
        char *s = strchr(p_copy + 1, '/');
        while (s) {
          *s = '\0';
          do_mkdir(p_copy, 0755);
          *s = '/';
          s = strchr(s + 1, '/');
        }
        if (do_mkdir(p_copy, mode & 0777) == 0) dirs_created++;
        kfree(p_copy);
      }
    } else if (S_ISREG(mode)) {
      /* Create parent dirs */
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
        ssize_t written = kernel_write(f, (const char *) file_data, filesize, &pos);
        if (written >= 0 && (size_t) written == filesize) {
          files_created++;
          printk(KERN_INFO INITRD_CLASS "Unpacked: %s (%u bytes)\n", full_path, filesize);
        } else {
          printk(KERN_ERR INITRD_CLASS "Write failed %s: %ld\n", full_path, (long) written);
        }
        vfs_close(f);
      } else {
        printk(KERN_ERR INITRD_CLASS "Open failed %s\n", full_path);
      }
    } else if (S_ISLNK(mode)) {
      char *target = kmalloc(filesize + 1);
      if (target) {
        memcpy(target, file_data, filesize);
        target[filesize] = '\0';
        if (do_symlink(target, full_path) == 0) {
          printk(KERN_INFO INITRD_CLASS "Unpacked symlink: %s -> %s\n", full_path, target);
        }
        kfree(target);
      }
    }

    p += cpio_align(110 + namesize);
    p += cpio_align(filesize);
  }

  printk(KERN_INFO INITRD_CLASS "Unpack summary: %d files, %d dirs, %d records.\n",
         files_created, dirs_created, records_found);
  return (records_found > 0) ? 0 : -EINVAL;
}

int initramfs_cpio_prober(const struct limine_file *file, lmm_type_t *out_type) {
  if (file->size < 6) return 0;

  uint8_t *p = (uint8_t *) file->address;
  uint8_t *end = p + file->size;
  uint8_t *search_end = p + CPIO_SEARCH_WINDOW;
  if (search_end > end - 6) search_end = end - 6;

  while (p < search_end) {
    if (memcmp(p, CPIO_NEWC_MAGIC, 6) == 0) {
      *out_type = LMM_TYPE_INITRD;
      return 80;
    }
    p++;
  }

  return 0;
}

void initramfs_init(const char *initrd_name) {
#ifdef CONFIG_INITRAMFS
  struct lmm_entry *entry = nullptr;

  if (initrd_name && initrd_name[0] != '\0') {
    entry = lmm_find_module(initrd_name);
  }

  if (!entry) {
    entry = lmm_find_module_by_type(LMM_TYPE_INITRD);
  }

  if (entry) {
    const int ret = initramfs_unpack((void *) entry->file->address, entry->file->size);
    if (ret < 0) printk(KERN_ERR INITRD_CLASS "initramfs unpack failed with error %s\n", errname(ret));
    else printk(KERN_INFO INITRD_CLASS "initramfs unpack success.\n");
  } else {
    printk(KERN_INFO INITRD_CLASS "No initramfs module found.\n");
  }
#endif
}
