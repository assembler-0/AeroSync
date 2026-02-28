/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/crypto.c
 * @brief Core Cryptography API and System Interface
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/crypto.h>
#include <aerosync/mutex.h>
#include <aerosync/panic.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <linux/list.h>
#include <fs/devtmpfs.h>
#include <aerosync/sysintf/char.h>

/* --- Core Algorithm Management --- */

static LIST_HEAD(crypto_alg_list);
static DEFINE_MUTEX(crypto_mutex);

int crypto_register_alg(struct crypto_alg *alg) {
  struct crypto_alg *entry;
    
  mutex_lock(&crypto_mutex);
  list_for_each_entry(entry, &crypto_alg_list, list) {
    if (strcmp(entry->driver_name, alg->driver_name) == 0) {
      mutex_unlock(&crypto_mutex);
      return -EEXIST;
    }
  }
    
  struct list_head *pos;
  list_for_each(pos, &crypto_alg_list) {
    entry = list_entry(pos, struct crypto_alg, list);
    if (alg->priority > entry->priority)
      break;
  }
  list_add_tail(&alg->list, pos);
    
  mutex_unlock(&crypto_mutex);
  printk(KERN_DEBUG CRYPTO_CLASS "registered algorithm: %s (%s)\n", alg->name, alg->driver_name);
  return 0;
}

int crypto_unregister_alg(struct crypto_alg *alg) {
  mutex_lock(&crypto_mutex);
  list_del(&alg->list);
  mutex_unlock(&crypto_mutex);
  return 0;
}

struct crypto_tfm *crypto_alloc_tfm(const char *name, enum crypto_alg_type type) {
  struct crypto_alg *alg = nullptr, *entry;
  struct crypto_tfm *tfm;
    
  mutex_lock(&crypto_mutex);
  list_for_each_entry(entry, &crypto_alg_list, list) {
    if (entry->type != type) continue;
    if (strcmp(entry->name, name) == 0 || strcmp(entry->driver_name, name) == 0) {
      alg = entry;
      break;
    }
  }
  mutex_unlock(&crypto_mutex);
    
  if (!alg) return nullptr;
        
  tfm = kmalloc(sizeof(struct crypto_tfm));
  if (!tfm) return nullptr;
        
  tfm->alg = alg;
  tfm->ctx = kmalloc(alg->ctx_size);
  if (!tfm->ctx) {
    kfree(tfm);
    return nullptr;
  }
    
  memset(tfm->ctx, 0, alg->ctx_size);
  if (alg->init) {
    if (alg->init(tfm->ctx) < 0) {
      kfree(tfm->ctx);
      kfree(tfm);
      return nullptr;
    }
  }
    
  return tfm;
}

void crypto_free_tfm(struct crypto_tfm *tfm) {
  if (!tfm) return;
  if (tfm->alg->exit) tfm->alg->exit(tfm->ctx);
  kfree(tfm->ctx);
  kfree(tfm);
}

void *crypto_tfm_ctx(struct crypto_tfm *tfm) {
  return tfm->ctx;
}

int crypto_shash_update(struct crypto_tfm *tfm, const uint8_t *data, size_t len) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_SHASH) return -EINVAL;
  return tfm->alg->shash.update(tfm->ctx, data, len);
}

int crypto_shash_final(struct crypto_tfm *tfm, uint8_t *out) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_SHASH) return -EINVAL;
  return tfm->alg->shash.final(tfm->ctx, out);
}

int crypto_shash_digest(struct crypto_tfm *tfm, const uint8_t *data, size_t len, uint8_t *out) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_SHASH) return -EINVAL;
  if (tfm->alg->shash.digest) return tfm->alg->shash.digest(tfm->ctx, data, len, out);
  int ret = tfm->alg->init(tfm->ctx);
  if (ret) return ret;
  ret = tfm->alg->shash.update(tfm->ctx, data, len);
  if (ret) return ret;
  return tfm->alg->shash.final(tfm->ctx, out);
}

size_t crypto_shash_digestsize(struct crypto_tfm *tfm) {
  return (tfm->alg->type == CRYPTO_ALG_TYPE_SHASH) ? tfm->alg->shash.digestsize : 0;
}

size_t crypto_shash_blocksize(struct crypto_tfm *tfm) {
  return (tfm->alg->type == CRYPTO_ALG_TYPE_SHASH) ? tfm->alg->shash.blocksize : 0;
}

int crypto_cipher_setkey(struct crypto_tfm *tfm, const uint8_t *key, size_t keylen) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_CIPHER) return -EINVAL;
  if (keylen < tfm->alg->cipher.min_keysize || keylen > tfm->alg->cipher.max_keysize) return -EINVAL;
  return tfm->alg->cipher.setkey(tfm->ctx, key, keylen);
}

int crypto_cipher_encrypt(struct crypto_tfm *tfm, uint8_t *dst, const uint8_t *src) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_CIPHER) return -EINVAL;
  return tfm->alg->cipher.encrypt(tfm->ctx, dst, src);
}

int crypto_cipher_decrypt(struct crypto_tfm *tfm, uint8_t *dst, const uint8_t *src) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_CIPHER) return -EINVAL;
  return tfm->alg->cipher.decrypt(tfm->ctx, dst, src);
}

int crypto_rng_generate(struct crypto_tfm *tfm, uint8_t *dst, size_t len) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_RNG) return -EINVAL;
  return tfm->alg->rng.generate(tfm->ctx, dst, len);
}

int crypto_rng_seed(struct crypto_tfm *tfm, const uint8_t *seed, size_t len) {
  if (tfm->alg->type != CRYPTO_ALG_TYPE_RNG) return -EINVAL;
  return tfm->alg->rng.seed(tfm->ctx, seed, len);
}

/* --- System Interface (/runtime/devices/crypto) --- */

static int crypto_dev_open(struct char_device *cdev) {
  (void)cdev;
  return 0;
}

static ssize_t crypto_dev_read(struct char_device *cdev, void *buf, size_t count, vfs_loff_t *ppos) {
  (void)cdev; (void)ppos;
  uint8_t *tmp = kmalloc(count);
  if (!tmp) return -ENOMEM;
  struct crypto_tfm *tfm = crypto_alloc_tfm("hw_rng", CRYPTO_ALG_TYPE_RNG);
  if (!tfm) tfm = crypto_alloc_tfm("sw_rng", CRYPTO_ALG_TYPE_RNG);
  if (tfm) {
    crypto_rng_generate(tfm, tmp, count);
    crypto_free_tfm(tfm);
  } else {
    kfree(tmp);
    return -ENODEV;
  }
  memcpy(buf, tmp, count);
  kfree(tmp);
  return count;
}

static struct char_operations crypto_ops = {
  .open = crypto_dev_open,
  .read = crypto_dev_read,
};

static struct char_device crypto_cdev;

int crypto_sysintf_init(void) {
  const char *path = CONFIG_CRYPTO_DEV_PATH;
  const char *devname = strrchr(path, '/');
  devname = devname ? devname + 1 : path;

  device_initialize(&crypto_cdev.dev);
  device_set_name(&crypto_cdev.dev, "%s", devname);
  crypto_cdev.ops = &crypto_ops;
  crypto_cdev.dev_num = MKDEV(10, 235); /* MISC_MAJOR style */

  int ret = char_device_register(&crypto_cdev);
  if (ret < 0) {
    printk(KERN_ERR HAL_CLASS "failed to register crypto character device: %d\n", ret);
    return ret;
  }
    
  printk(KERN_INFO HAL_CLASS "crypto interface registered via driver model at %s\n", path);
  return 0;
}
