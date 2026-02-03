/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/string.c
 * @brief string manipulation functions
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <lib/string.h>
#include <aerosync/fkx/fkx.h>
#include <mm/slub.h>
#include <arch/x86_64/features/features.h>

bool is_word_boundary(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\0';
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
  size_t n = (size_t) -1;
  const char *p = str;
  __asm__ volatile("cld\n\t"
    "repne scasb"
    : "+D"(p), "+c"(n)
    : "a"((unsigned char) 0)
    : "memory");
  return (int) ((size_t) -2 - n);
}

int strnlen(const char *str, const size_t max) {
  if (!str || max == 0) return 0;
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
  dest[i] = '\0';
}

void strcpy(char *dest, const char *src) {
  if (!dest || !src)
    return;
  // Optimize for 64-bit aligned copies when possible
  if (((uintptr_t) dest & 7) == 0 && ((uintptr_t) src & 7) == 0) {
    uint64_t *d64 = (uint64_t *) dest;
    const uint64_t *s64 = (const uint64_t *) src;

    uint64_t val;
    while ((val = *s64++) != 0) {
      // Check if any byte in the 64-bit value is zero
      if ((val & 0xFF00000000000000ULL) == 0 ||
          (val & 0x00FF000000000000ULL) == 0 ||
          (val & 0x0000FF0000000000ULL) == 0 ||
          (val & 0x000000FF00000000ULL) == 0 ||
          (val & 0x00000000FF000000ULL) == 0 ||
          (val & 0x0000000000FF0000ULL) == 0 ||
          (val & 0x000000000000FF00ULL) == 0 ||
          (val & 0x00000000000000FFULL) == 0) {
        // Found null terminator, fall back to byte copy
        char *d = (char *) d64;
        const char *s = (const char *) (s64 - 1);
        while ((*d++ = *s++));
        return;
      }
      *d64++ = val;
    }
    *(char *) d64 = '\0';
  } else {
    // Original byte-by-byte copy for unaligned data
    while ((*dest++ = *src++));
  }
}

void strcat(char *dest, const char *src) {
  if (!dest || !src)
    return;
  while (*dest)
    dest++;
  strcpy(dest, src); // Reuse optimized strcpy
}


char *kstrdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *new = kmalloc(len);
  if (new) memcpy(new, s, len);
  return new;
}

void htoa(uint64_t n, char *buffer) {
  if (!buffer)
    return;

  __attribute__((nonstring)) static const char hex_chars[16] =
      "0123456789ABCDEF";

  buffer[0] = '0';
  buffer[1] = 'x';

  // Unroll the loop for better performance
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

  // Use faster division by avoiding modulo when possible
  while (n >= 10) {
    uint64_t q = n / 10;
    *--p = '0' + (n - q * 10); // Faster than n % 10
    n = q;
  }
  *--p = '0' + n;

  strcpy(buffer, p);
}

/**
 * strspn - Calculate the length of the initial substring of @s which only
 * 	contain letters in @accept
 * @s: The string to be searched
 * @accept: The string to search for
 */
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

/**
 * strpbrk - Find the first occurrence of a set of characters
 * @cs: The string to be searched
 * @ct: The characters to search for
 */
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

/**
 * strsep - Split a string into tokens
 * @s: The string to be searched
 * @ct: The characters to search for
 *
 * strsep() updates @s to point after the token, ready for the next call.
 *
 * It returns empty tokens, too, behaving exactly like the libc function
 * of that name. In fact, it was stolen from glibc2 and de-fancy-fied.
 * Same semantics, slimmer shape. ;)
 */
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

void *memset(void *s, int c, size_t n) {
  if (n == 0) return s;

  cpu_features_t *features = get_cpu_features();
  if (features->fsrm || (features->erms && n >= 64)) {
    __asm__ volatile("cld\n\t"
      "rep stosb"
      : "+D"(s), "+c"(n)
      : "a"((unsigned char) c)
      : "memory");
    return s;
  }

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

  cpu_features_t *features = get_cpu_features();
  if (features->fsrm || (features->erms && n >= 64)) {
    __asm__ volatile("cld\n\t"
      "rep movsb"
      : "+D"(d), "+S"(s), "+c"(n)
      :
      : "memory");
    return d;
  }

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

static inline char to_lower(char c) {
  if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
  return c;
}

int strcasecmp(const char *s1, const char *s2) {
  while (*s1 && (to_lower(*s1) == to_lower(*s2))) {
    s1++;
    s2++;
  }
  return (unsigned char) to_lower(*s1) - (unsigned char) to_lower(*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
  if (n == 0) return 0;
  while (n-- > 0 && *s1 && (to_lower(*s1) == to_lower(*s2))) {
    if (n == 0 || !*s1) break;
    s1++;
    s2++;
  }
  return (unsigned char) to_lower(*s1) - (unsigned char) to_lower(*s2);
}

size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size > 0) {
    size_t copy_len = (len >= size) ? size - 1 : len;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
  }
  return len;
}

uint64_t strtoul(const char *nptr, char **endptr, int base) {
  const char *s = nptr;
  uint64_t acc = 0;
  int c;

  while (is_word_boundary(*s)) s++;

  if (base == 0) {
    if (*s == '0') {
      s++;
      if (*s == 'x' || *s == 'X') {
        s++;
        base = 16;
      } else {
        base = 8;
      }
    } else {
      base = 10;
    }
  } else if (base == 16) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  }

  for (;;) {
    c = (unsigned char) *s;
    if (c >= '0' && c <= '9') c -= '0';
    else if (c >= 'A' && c <= 'Z') c -= 'A' - 10;
    else if (c >= 'a' && c <= 'z') c -= 'a' - 10;
    else break;

    if (c >= base) break;
    acc = acc * base + c;
    s++;
  }

  if (endptr) *endptr = (char *) s;
  return acc;
}

void *memchr(const void *s, int c, size_t n) {
  if (n == 0) return nullptr;
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
}

EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memset32);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memchr);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strnlen);
