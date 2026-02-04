// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/string.c
 * @brief string manipulation functions
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <lib/string.h>
#include <aerosync/fkx/fkx.h>
#include <mm/slub.h>
#include <arch/x86_64/features/features.h>
#include <aerosync/ctype.h>
#include <aerosync/errno.h>
#include <aerosync/stdarg.h>

#ifndef INT_MAX
#define INT_MAX 2147483647
#endif

#ifdef CONFIG_STRING_CHECK
#define STRING_CHECK(cond) if (!(cond)) return
#define STRING_CHECK_RET(cond, ret) if (!(cond)) return (ret)
#else
#define STRING_CHECK(cond)
#define STRING_CHECK_RET(cond, ret)
#endif

bool is_word_boundary(char c) {
  return isspace(c) || c == '\0';
}

bool find(const char *buff, const char *pattern) {
  if (buff == nullptr || pattern == nullptr || *pattern == '\0') {
    return false;
  }

  const size_t pattern_len = strlen(pattern);
  const char *ptr = buff;

  while ((ptr = strstr(ptr, pattern)) != nullptr) {
    bool at_start = (ptr == buff) || is_word_boundary(*(ptr - 1));
    bool at_end = is_word_boundary(*(ptr + pattern_len));

    if (at_start && at_end) {
      return true;
    }

    ptr++;
  }

  return false;
}

char *strstr(const char *haystack, const char *needle) {
  if (*needle == '\0') {
    return (char *) haystack;
  }

  for (const char *h_ptr = haystack; *h_ptr != '\0'; h_ptr++) {
    if (*h_ptr == *needle) {
      const char *n_ptr = needle;
      const char *current_h = h_ptr;

      while (*n_ptr != '\0' && *current_h != '\0' && *current_h == *n_ptr) {
        current_h++;
        n_ptr++;
      }

      if (*n_ptr == '\0') {
        return (char *) h_ptr;
      }
    }
  }

  return nullptr;
}

#ifdef CONFIG_LIBC_STRING_FULL
char *strnstr(const char *haystack, const char *needle, size_t len) {
  size_t l2;
  l2 = strlen(needle);
  if (!l2)
    return (char *) haystack;
  while (len >= l2 && *haystack) {
    len--;
    if (!memcmp(haystack, needle, l2))
      return (char *) haystack;
    haystack++;
  }
  return nullptr;
}
#endif

int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i])
      return (unsigned char) a[i] - (unsigned char) b[i];
    if (a[i] == '\0')
      return 0;
  }
  return 0;
}

int strcmp(const char *a, const char *b) {
  while (*a && (*a == *b)) {
    a++;
    b++;
  }
  return (unsigned char) *a - (unsigned char) *b;
}

int strlen(const char *str) {
  if (!str) return 0;
#ifdef CONFIG_OPTIMIZED_STRING
  size_t n = (size_t) -1;
  const char *p = str;
  __asm__ volatile("cld\n\t"
    "repne scasb"
    : "+D"(p), "+c"(n)
    : "a"((unsigned char) 0)
    : "memory");
  return (int) ((size_t) -2 - n);
#else
  const char *s;
  for (s = str; *s; ++s);
  return (int)(s - str);
#endif
}

int strnlen(const char *str, const size_t max) {
  if (!str || max == 0) return 0;
#ifdef CONFIG_OPTIMIZED_STRING
  size_t n = max;
  const char *p = str;
  __asm__ volatile("cld\n\t"
    "repne scasb\n\t"
    "jnz 1f\n\t"
    "inc %%rcx\n\t"
    "1:"
    : "+D"(p), "+c"(n)
    : "a"((unsigned char) 0)
    : "memory");
  return (int) (max - n);
#else
  const char *s;
  for (s = str; max-- && *s; ++s);
  return (int)(s - str);
#endif
}

char *strchr(char *str, int c) {
  if (!str) return nullptr;
  return memchr(str, c, strlen(str) + 1);
}

void strncpy(char *dest, const char *src, size_t max_len) {
  if (!dest || !src)
    return;
  size_t i = 0;
  for (; i + 1 < max_len && src[i]; i++)
    dest[i] = src[i];
  if (max_len > 0)
    dest[i] = '\0';
}

void strcpy(char *dest, const char *src) {
  if (!dest || !src)
    return;
#ifdef CONFIG_OPTIMIZED_STRING
  if (((uintptr_t) dest & 7) == 0 && ((uintptr_t) src & 7) == 0) {
    uint64_t *d64 = (uint64_t *) dest;
    const uint64_t *s64 = (const uint64_t *) src;

    uint64_t val;
    while ((val = *s64++) != 0) {
      if ((val & 0xFF00000000000000ULL) == 0 ||
          (val & 0x00FF000000000000ULL) == 0 ||
          (val & 0x0000FF0000000000ULL) == 0 ||
          (val & 0x000000FF00000000ULL) == 0 ||
          (val & 0x00000000FF000000ULL) == 0 ||
          (val & 0x0000000000FF0000ULL) == 0 ||
          (val & 0x000000000000FF00ULL) == 0 ||
          (val & 0x00000000000000FFULL) == 0) {
        char *d = (char *) d64;
        const char *s = (const char *) (s64 - 1);
        while ((*d++ = *s++));
        return;
      }
      *d64++ = val;
    }
    *(char *) d64 = '\0';
  } else {
    while ((*dest++ = *src++));
  }
#else
  while ((*dest++ = *src++));
#endif
}

void strcat(char *dest, const char *src) {
  if (!dest || !src)
    return;
  while (*dest)
    dest++;
  strcpy(dest, src);
}

#ifdef CONFIG_LIBC_STRING_FULL
void strncat(char *dest, const char *src, size_t count) {
  char *tmp = dest;
  while (*tmp)
    tmp++;
  while (count--) {
    if (!(*tmp++ = *src++))
      return;
  }
  *tmp = '\0';
}
#endif

size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size > 0) {
    size_t copy_len = (len >= size) ? size - 1 : len;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
  }
  return len;
}

size_t strlcat(char *dst, const char *src, size_t size) {
  size_t dlen = strnlen(dst, size);
  size_t slen = strlen(src);
  if (dlen == size)
    return size + slen;
  if (slen < size - dlen)
    memcpy(dst + dlen, src, slen + 1);
  else {
    memcpy(dst + dlen, src, size - dlen - 1);
    dst[size - 1] = '\0';
  }
  return dlen + slen;
}

char *kstrdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *new = kmalloc(len);
  if (new) memcpy(new, s, len);
  return new;
}

char *strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *new = kmalloc(len + 1);
  if (new) {
    memcpy(new, s, len);
    new[len] = '\0';
  }
  return new;
}

void htoa(uint64_t n, char *buffer) {
  if (!buffer)
    return;

  __attribute__((nonstring)) static const char hex_chars[16] =
      "0123456789ABCDEF";

  buffer[0] = '0';
  buffer[1] = 'x';

  buffer[2] = hex_chars[(n >> 60) & 0xF];
  buffer[3] = hex_chars[(n >> 56) & 0xF];
  buffer[4] = hex_chars[(n >> 52) & 0xF];
  buffer[5] = hex_chars[(n >> 48) & 0xF];
  buffer[6] = hex_chars[(n >> 44) & 0xF];
  buffer[7] = hex_chars[(n >> 40) & 0xF];
  buffer[8] = hex_chars[(n >> 36) & 0xF];
  buffer[9] = hex_chars[(n >> 32) & 0xF];
  buffer[10] = hex_chars[(n >> 28) & 0xF];
  buffer[11] = hex_chars[(n >> 24) & 0xF];
  buffer[12] = hex_chars[(n >> 20) & 0xF];
  buffer[13] = hex_chars[(n >> 16) & 0xF];
  buffer[14] = hex_chars[(n >> 12) & 0xF];
  buffer[15] = hex_chars[(n >> 8) & 0xF];
  buffer[16] = hex_chars[(n >> 4) & 0xF];
  buffer[17] = hex_chars[n & 0xF];
  buffer[18] = '\0';
}

void itoa(uint64_t n, char *buffer) {
  if (n == 0) {
    buffer[0] = '0';
    buffer[1] = '\0';
    return;
  }

  char temp_buffer[21];
  char *p = &temp_buffer[20];
  *p = '\0';

  while (n >= 10) {
    uint64_t q = n / 10;
    *--p = '0' + (n - q * 10);
    n = q;
  }
  *--p = '0' + n;

  strcpy(buffer, p);
}

size_t strspn(const char *s, const char *accept) {
  const char *p;
  const char *a;
  size_t count = 0;

  for (p = s; *p != '\0'; ++p) {
    for (a = accept; *a != '\0'; ++a) {
      if (*p == *a)
        break;
    }
    if (*a == '\0')
      return count;
    ++count;
  }

  return count;
}

#ifdef CONFIG_LIBC_STRING_FULL
size_t strcspn(const char *s, const char *reject) {
  const char *p;
  const char *r;
  size_t count = 0;

  for (p = s; *p != '\0'; ++p) {
    for (r = reject; *r != '\0'; ++r) {
      if (*p == *r)
        return count;
    }
    ++count;
  }

  return count;
}
#endif

char *strpbrk(const char *cs, const char *ct) {
  const char *sc1, *sc2;

  for (sc1 = cs; *sc1 != '\0'; ++sc1) {
    for (sc2 = ct; *sc2 != '\0'; ++sc2) {
      if (*sc1 == *sc2)
        return (char *) sc1;
    }
  }
  return nullptr;
}

char *strsep(char **s, const char *ct) {
  char *sbegin = *s, *end;

  if (sbegin == nullptr)
    return nullptr;

  end = strpbrk(sbegin, ct);
  if (end)
    *end++ = '\0';
  *s = end;

  return sbegin;
}

char *strrchr(const char *s, int c) {
  const char *last_occurrence = nullptr;
  do {
    if ((unsigned char) *s == (unsigned char) c) {
      last_occurrence = s;
    }
  } while (*s++);
  return (char *) last_occurrence;
}

#ifdef CONFIG_LIBC_STRING_FULL
char *strtok(char *s, const char *ct) {
  static char *last;
  return strtok_r(s, ct, &last);
}

char *strtok_r(char *s, const char *ct, char **last) {
  char *sbegin, *send;

  sbegin = s ? s : *last;
  if (!sbegin)
    return nullptr;

  sbegin += strspn(sbegin, ct);
  if (*sbegin == '\0') {
    *last = nullptr;
    return nullptr;
  }

  send = strpbrk(sbegin, ct);
  if (send && *send != '\0')
    *send++ = '\0';

  *last = send;
  return sbegin;
}
#endif

void *memset(void *s, int c, size_t n) {
  if (n == 0) return s;

#ifdef CONFIG_OPTIMIZED_STRING
  cpu_features_t *features = get_cpu_features();
  if (features->fsrm || (features->erms && n >= 64)) {
    __asm__ volatile("cld\n\t"
      "rep stosb"
      : "+D"(s), "+c"(n)
      : "a"((unsigned char) c)
      : "memory");
    return s;
  }
#endif

  unsigned char *mem = (unsigned char *) s;
  unsigned char x = (unsigned char) c;

  if (n < 32) {
    while (n--) *mem++ = x;
    return s;
  }

  while ((uintptr_t) mem & 7) {
    *mem++ = x;
    n--;
  }

  uint64_t pattern = x;
  pattern |= pattern << 8;
  pattern |= pattern << 16;
  pattern |= pattern << 32;

  size_t num_words = n / 8;
  uint64_t *p64 = (uint64_t *) mem;

  while (num_words >= 4) {
    p64[0] = pattern;
    p64[1] = pattern;
    p64[2] = pattern;
    p64[3] = pattern;
    p64 += 4;
    num_words -= 4;
  }
  while (num_words--) {
    *p64++ = pattern;
  }

  mem = (unsigned char *) p64;
  n &= 7;
  while (n--) {
    *mem++ = x;
  }

  return s;
}

void *memcpy(void *d, const void *s, size_t n) {
  if (n == 0) return d;

#ifdef CONFIG_OPTIMIZED_STRING
  cpu_features_t *features = get_cpu_features();
  if (features->fsrm || (features->erms && n >= 64)) {
    __asm__ volatile("cld\n\t"
      "rep movsb"
      : "+D"(d), "+S"(s), "+c"(n)
      :
      : "memory");
    return d;
  }
#endif

  unsigned char *dst = (unsigned char *) d;
  const unsigned char *src = (const unsigned char *) s;

  if (n < 32) {
    if (n >= 16) {
      *(uint64_t *) dst = *(const uint64_t *) src;
      *(uint64_t *) (dst + 8) = *(const uint64_t *) (src + 8);
      *(uint64_t *) (dst + n - 16) = *(const uint64_t *) (src + n - 16);
      *(uint64_t *) (dst + n - 8) = *(const uint64_t *) (src + n - 8);
      return d;
    }
    if (n >= 8) {
      *(uint64_t *) dst = *(const uint64_t *) src;
      *(uint64_t *) (dst + n - 8) = *(const uint64_t *) (src + n - 8);
      return d;
    }
    if (n >= 4) {
      *(uint32_t *) dst = *(const uint32_t *) src;
      *(uint32_t *) (dst + n - 4) = *(const uint32_t *) (src + n - 4);
      return d;
    }
    while (n--) *dst++ = *src++;
    return d;
  }

  while ((uintptr_t) dst & 7) {
    *dst++ = *src++;
    n--;
  }

  size_t num_words = n / 8;
  uint64_t *d64 = (uint64_t *) dst;
  const uint64_t *s64 = (const uint64_t *) src;

  while (num_words >= 4) {
    d64[0] = s64[0];
    d64[1] = s64[1];
    d64[2] = s64[2];
    d64[3] = s64[3];
    d64 += 4;
    s64 += 4;
    num_words -= 4;
  }
  while (num_words--) {
    *d64++ = *s64++;
  }

  dst = (unsigned char *) d64;
  src = (const unsigned char *) s64;
  n &= 7;
  while (n--) {
    *dst++ = *src++;
  }

  return d;
}

void *memmove(void *dest, const void *src, size_t n) {
  if (n == 0 || dest == src) return dest;

  if (dest < src) {
    return memcpy(dest, src, n);
  }

#ifdef CONFIG_OPTIMIZED_STRING
  cpu_features_t *features = get_cpu_features();
  if (features->fsrm || (features->erms && n >= 64)) {
    void *d_end = (char *) dest + n - 1;
    const void *s_end = (const char *) src + n - 1;
    __asm__ volatile("std\n\t"
      "rep movsb\n\t"
      "cld"
      : "+D"(d_end), "+S"(s_end), "+c"(n)
      :
      : "memory");
    return dest;
  }
#endif

  unsigned char *d = (unsigned char *) dest + n;
  const unsigned char *s = (const unsigned char *) src + n;

  if (n < 32) {
    while (n--) *--d = *--s;
    return dest;
  }

  while ((uintptr_t) d & 7) {
    *--d = *--s;
    n--;
  }

  size_t num_words = n / 8;
  uint64_t *d64 = (uint64_t *) d;
  const uint64_t *s64 = (const uint64_t *) s;

  while (num_words >= 4) {
    d64 -= 4;
    s64 -= 4;
    d64[3] = s64[3];
    d64[2] = s64[2];
    d64[1] = s64[1];
    d64[0] = s64[0];
    num_words -= 4;
  }
  while (num_words--) {
    *--d64 = *--s64;
  }

  d = (unsigned char *) d64;
  s = (const unsigned char *) s64;
  n &= 7;
  while (n--) {
    *--d = *--s;
  }

  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  if (n == 0) return 0;

  const unsigned char *p1 = (const unsigned char *) s1;
  const unsigned char *p2 = (const unsigned char *) s2;

  if (n >= 8) {
    while ((uintptr_t) p1 & 7) {
      if (*p1 != *p2) return (int) *p1 - (int) *p2;
      p1++;
      p2++;
      n--;
    }

    const uint64_t *q1 = (const uint64_t *) p1;
    const uint64_t *q2 = (const uint64_t *) p2;

    while (n >= 8) {
      if (*q1 != *q2) break;
      q1++;
      q2++;
      n -= 8;
    }

    p1 = (const unsigned char *) q1;
    p2 = (const unsigned char *) q2;
  }

  while (n--) {
    if (*p1 != *p2) return (int) *p1 - (int) *p2;
    p1++;
    p2++;
  }

  return 0;
}

void *memset32(void *s, uint32_t val, size_t n) {
  if (n == 0) return s;

  uint32_t *p32 = (uint32_t *) s;

  while (n > 0 && ((uintptr_t) p32 & 7)) {
    *p32++ = val;
    n--;
  }

  uint64_t val64 = ((uint64_t) val << 32) | val;
  uint64_t *p64 = (uint64_t *) p32;
  size_t n64 = n / 2;

  while (n64 >= 4) {
    p64[0] = val64;
    p64[1] = val64;
    p64[2] = val64;
    p64[3] = val64;
    p64 += 4;
    n64 -= 4;
  }
  while (n64--) {
    *p64++ = val64;
  }

  if (n & 1) {
    *(uint32_t *) p64 = val;
  }

  return s;
}

#ifdef CONFIG_LIBC_STRING_FULL
void *memscan(void *addr, int c, size_t size) {
  unsigned char *p = addr;
  while (size) {
    if (*p == (unsigned char)c)
      return (void *)p;
    p++;
    size--;
  }
  return (void *)p;
}
#endif

int strcasecmp(const char *s1, const char *s2) {
  int c1, c2;
  do {
    c1 = tolower(*s1++);
    c2 = tolower(*s2++);
  } while (c1 == c2 && c1 != 0);
  return c1 - c2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
  int c1, c2;
  if (!n) return 0;
  do {
    c1 = tolower(*s1++);
    c2 = tolower(*s2++);
  } while (--n && c1 == c2 && c1 != 0);
  return c1 - c2;
}

unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base) {
  unsigned long long result = 0;

  if (!base)
    base = 10;

  if (base == 16 && cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X'))
    cp += 2;

  while (isxdigit(*cp)) {
    unsigned int value;

    value = isdigit(*cp) ? *cp - '0' : toupper(*cp) - 'A' + 10;
    if (value >= base)
      break;
    result = result * base + value;
    cp++;
  }

  if (endp)
    *endp = (char *) cp;

  return result;
}

long long simple_strtoll(const char *cp, char **endp, unsigned int base) {
  if (*cp == '-')
    return -simple_strtoull(cp + 1, endp, base);

  return simple_strtoull(cp, endp, base);
}

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base) {
  return (unsigned long) simple_strtoull(cp, endp, base);
}

long simple_strtol(const char *cp, char **endp, unsigned int base) {
  if (*cp == '-')
    return -simple_strtoul(cp + 1, endp, base);

  return simple_strtoul(cp, endp, base);
}

uint64_t strtoul(const char *nptr, char **endptr, int base) {
  return simple_strtoul(nptr, endptr, base);
}

long strtol(const char *cp, char **endp, unsigned int base) {
  return simple_strtol(cp, endp, base);
}

static int _kstrtoull(const char *s, unsigned int base, unsigned long long *res) {
  unsigned long long acc;
  char *endp;

  while (isspace(*s))
    s++;

  acc = simple_strtoull(s, &endp, base);
  if (endp == s)
    return -EINVAL;

  if (*endp && !isspace(*endp))
    return -EINVAL;

  *res = acc;
  return 0;
}

int kstrtoull(const char *s, unsigned int base, unsigned long long *res) {
  return _kstrtoull(s, base, res);
}

int kstrtoll(const char *s, unsigned int base, long long *res) {
  unsigned long long acc;
  int neg = 0;
  int ret;

  while (isspace(*s))
    s++;

  if (*s == '-') {
    neg = 1;
    s++;
  }

  ret = _kstrtoull(s, base, &acc);
  if (ret < 0)
    return ret;

  if (neg)
    *res = -acc;
  else
    *res = acc;
  return 0;
}

int kstrtoul(const char *s, unsigned int base, unsigned long *res) {
  unsigned long long tmp;
  int ret;

  ret = kstrtoull(s, base, &tmp);
  if (ret < 0)
    return ret;

  if (tmp != (unsigned long long) (unsigned long) tmp)
    return -ERANGE;

  *res = (unsigned long) tmp;
  return 0;
}

int kstrtol(const char *s, unsigned int base, long *res) {
  long long tmp;
  int ret;

  ret = kstrtoll(s, base, &tmp);
  if (ret < 0)
    return ret;

  if (tmp != (long long) (long) tmp)
    return -ERANGE;

  *res = (long) tmp;
  return 0;
}

int kstrtouint(const char *s, unsigned int base, unsigned int *res) {
  unsigned long long tmp;
  int ret;

  ret = kstrtoull(s, base, &tmp);
  if (ret < 0)
    return ret;

  if (tmp != (unsigned long long) (unsigned int) tmp)
    return -ERANGE;

  *res = (unsigned int) tmp;
  return 0;
}

int kstrtoint(const char *s, unsigned int base, int *res) {
  long long tmp;
  int ret;

  ret = kstrtoll(s, base, &tmp);
  if (ret < 0)
    return ret;

  if (tmp != (long long) (int) tmp)
    return -ERANGE;

  *res = (int) tmp;
  return 0;
}

int kstrtou8(const char *s, unsigned int base, uint8_t *res) {
  unsigned long long tmp;
  int ret;
  ret = kstrtoull(s, base, &tmp);
  if (ret < 0) return ret;
  if (tmp > (uint8_t)-1) return -ERANGE;
  *res = (uint8_t)tmp;
  return 0;
}

int kstrtos8(const char *s, unsigned int base, int8_t *res) {
  long long tmp;
  int ret;
  ret = kstrtoll(s, base, &tmp);
  if (ret < 0) return ret;
  if (tmp < (int8_t)-128 || tmp > 127) return -ERANGE;
  *res = (int8_t)tmp;
  return 0;
}

int kstrtou16(const char *s, unsigned int base, uint16_t *res) {
  unsigned long long tmp;
  int ret;
  ret = kstrtoull(s, base, &tmp);
  if (ret < 0) return ret;
  if (tmp > (uint16_t)-1) return -ERANGE;
  *res = (uint16_t)tmp;
  return 0;
}

int kstrtos16(const char *s, unsigned int base, int16_t *res) {
  long long tmp;
  int ret;
  ret = kstrtoll(s, base, &tmp);
  if (ret < 0) return ret;
  if (tmp < (int16_t)-32768 || tmp > 32767) return -ERANGE;
  *res = (int16_t)tmp;
  return 0;
}

int kstrtou32(const char *s, unsigned int base, uint32_t *res) {
  unsigned long long tmp;
  int ret;
  ret = kstrtoull(s, base, &tmp);
  if (ret < 0) return ret;
  if (tmp > (uint32_t)-1) return -ERANGE;
  *res = (uint32_t)tmp;
  return 0;
}

int kstrtos32(const char *s, unsigned int base, int32_t *res) {
  long long tmp;
  int ret;
  ret = kstrtoll(s, base, &tmp);
  if (ret < 0) return ret;
  if (tmp < (int32_t)-2147483648LL || tmp > 2147483647LL) return -ERANGE;
  *res = (int32_t)tmp;
  return 0;
}

long long atoll(const char *s) {
  return simple_strtoll(s, nullptr, 10);
}

long atol(const char *s) {
  return simple_strtol(s, nullptr, 10);
}

int atoi(const char *s) {
  return (int) simple_strtol(s, nullptr, 10);
}

void *memchr(const void *s, int c, size_t n) {
  if (n == 0) return nullptr;
#ifdef CONFIG_OPTIMIZED_STRING
  void *res;
  __asm__ volatile("cld\n\t"
    "repne scasb\n\t"
    "jnz 1f\n\t"
    "lea -1(%%rdi), %0\n\t"
    "jmp 2f\n\t"
    "1: xor %0, %0\n\t"
    "2:"
    : "=r"(res), "+D"(s), "+c"(n)
    : "a"((unsigned char) c)
    : "memory");
  return res;
#else
  const unsigned char *p = (const unsigned char *)s;
  while (n--) {
    if (*p == (unsigned char)c)
      return (void *)p;
    p++;
  }
  return nullptr;
#endif
}

#ifdef CONFIG_STRING_ADVANCED
void *memrchr(const void *s, int c, size_t n) {
  if (n == 0) return nullptr;
  const unsigned char *p = (const unsigned char *)s + n - 1;
  while (n--) {
    if (*p == (unsigned char)c)
      return (void *)p;
    p--;
  }
  return nullptr;
}

void *memmem(const void *haystack, size_t hlen, const void *needle, size_t nlen) {
  if (nlen == 0) return (void *)haystack;
  if (hlen < nlen) return nullptr;
  
  const unsigned char *h = (const unsigned char *)haystack;
  const unsigned char *n = (const unsigned char *)needle;
  
  for (size_t i = 0; i <= hlen - nlen; i++) {
    if (memcmp(h + i, n, nlen) == 0)
      return (void *)(h + i);
  }
  return nullptr;
}

void memswap(void *a, void *b, size_t n) {
  unsigned char *pa = (unsigned char *)a;
  unsigned char *pb = (unsigned char *)b;
  
  while (n >= 8) {
    uint64_t tmp = *(uint64_t *)pa;
    *(uint64_t *)pa = *(uint64_t *)pb;
    *(uint64_t *)pb = tmp;
    pa += 8; pb += 8; n -= 8;
  }
  
  while (n--) {
    unsigned char tmp = *pa;
    *pa++ = *pb;
    *pb++ = tmp;
  }
}
#endif

#ifdef CONFIG_STRING_CRYPTO
int memcmp_const_time(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;
  unsigned char diff = 0;
  
  while (n--) {
    diff |= *p1++ ^ *p2++;
  }
  
  return diff;
}

void explicit_bzero(void *s, size_t n) {
  volatile unsigned char *p = (volatile unsigned char *)s;
  while (n--) *p++ = 0;
}
#endif

#ifdef CONFIG_STRING_PATTERN
int fnmatch(const char *pattern, const char *string, int flags) {
  const char *p = pattern, *s = string;
  
  while (*p) {
    switch (*p) {
    case '*':
      if (!*++p) return 0;
      while (*s) {
        if (!fnmatch(p, s, flags)) return 0;
        s++;
      }
      return 1;
    case '?':
      if (!*s++) return 1;
      break;
    case '[':
      {
        int not = (*++p == '!');
        if (not) p++;
        int match = 0;
        while (*p && *p != ']') {
          if (*p == *s) match = 1;
          p++;
        }
        if (!*p) return 1;
        if (match == not) return 1;
        if (!*s++) return 1;
      }
      break;
    default:
      if (*p != *s) return 1;
      s++;
      break;
    }
    p++;
  }
  return *s != 0;
}

int strverscmp(const char *s1, const char *s2) {
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;
  
  while (*p1 == *p2) {
    if (*p1 == '\0') return 0;
    p1++; p2++;
  }
  
  if (isdigit(*p1) && isdigit(*p2)) {
    int val1 = 0, val2 = 0;
    while (isdigit(*p1)) val1 = val1 * 10 + (*p1++ - '0');
    while (isdigit(*p2)) val2 = val2 * 10 + (*p2++ - '0');
    return val1 - val2;
  }
  
  return *p1 - *p2;
}
#endif

#ifdef CONFIG_STRING_FLOAT
double strtod(const char *nptr, char **endptr) {
  const char *s = nptr;
  double result = 0.0;
  int sign = 1;
  
  while (isspace(*s)) s++;
  
  if (*s == '-') { sign = -1; s++; }
  else if (*s == '+') s++;
  
  while (isdigit(*s)) {
    result = result * 10.0 + (*s - '0');
    s++;
  }
  
  if (*s == '.') {
    s++;
    double frac = 0.1;
    while (isdigit(*s)) {
      result += (*s - '0') * frac;
      frac *= 0.1;
      s++;
    }
  }
  
  if (*s == 'e' || *s == 'E') {
    s++;
    int exp_sign = 1;
    int exp = 0;
    
    if (*s == '-') { exp_sign = -1; s++; }
    else if (*s == '+') s++;
    
    while (isdigit(*s)) {
      exp = exp * 10 + (*s - '0');
      s++;
    }
    
    double pow10 = 1.0;
    for (int i = 0; i < exp; i++) pow10 *= 10.0;
    
    if (exp_sign == 1) result *= pow10;
    else result /= pow10;
  }
  
  if (endptr) *endptr = (char *)s;
  return sign * result;
}

float strtof(const char *nptr, char **endptr) {
  return (float)strtod(nptr, endptr);
}
#endif

EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memset32);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memchr);
#ifdef CONFIG_STRING_ADVANCED
EXPORT_SYMBOL(memrchr);
EXPORT_SYMBOL(memmem);
EXPORT_SYMBOL(memswap);
#endif
#ifdef CONFIG_STRING_CRYPTO
EXPORT_SYMBOL(memcmp_const_time);
EXPORT_SYMBOL(explicit_bzero);
#endif
#ifdef CONFIG_LIBC_STRING_FULL
EXPORT_SYMBOL(memscan);
#endif
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strnlen);
EXPORT_SYMBOL(strlcpy);
EXPORT_SYMBOL(strlcat);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strrchr);
EXPORT_SYMBOL(strstr);
#ifdef CONFIG_LIBC_STRING_FULL
EXPORT_SYMBOL(strnstr);
#endif
EXPORT_SYMBOL(strpbrk);
EXPORT_SYMBOL(strsep);
EXPORT_SYMBOL(strspn);
#ifdef CONFIG_LIBC_STRING_FULL
EXPORT_SYMBOL(strcspn);
EXPORT_SYMBOL(strtok);
EXPORT_SYMBOL(strtok_r);
#endif
EXPORT_SYMBOL(strcasecmp);
EXPORT_SYMBOL(strncasecmp);
EXPORT_SYMBOL(simple_strtoul);
EXPORT_SYMBOL(simple_strtol);
EXPORT_SYMBOL(simple_strtoull);
EXPORT_SYMBOL(simple_strtoll);
EXPORT_SYMBOL(kstrtoul);
EXPORT_SYMBOL(kstrtol);
EXPORT_SYMBOL(kstrtoull);
EXPORT_SYMBOL(kstrtoll);
EXPORT_SYMBOL(kstrtouint);
EXPORT_SYMBOL(kstrtoint);
EXPORT_SYMBOL(kstrtou8);
EXPORT_SYMBOL(kstrtos8);
EXPORT_SYMBOL(kstrtou16);
EXPORT_SYMBOL(kstrtos16);
EXPORT_SYMBOL(kstrtou32);
EXPORT_SYMBOL(kstrtos32);
EXPORT_SYMBOL(atoi);
EXPORT_SYMBOL(atol);
EXPORT_SYMBOL(atoll);
EXPORT_SYMBOL(strndup);
#ifdef CONFIG_STRING_SCANF
EXPORT_SYMBOL(vsscanf);
EXPORT_SYMBOL(sscanf);
#endif
#ifdef CONFIG_STRING_PATTERN
EXPORT_SYMBOL(fnmatch);
EXPORT_SYMBOL(strverscmp);
#endif
#ifdef CONFIG_STRING_FLOAT
EXPORT_SYMBOL(strtod);
EXPORT_SYMBOL(strtof);
#endif