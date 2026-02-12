/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <aerosync/types.h>
#include <aerosync/stdarg.h>

/* String Manipulation */
int strncmp(const char *a, const char *b, size_t n);
int strcmp(const char *a, const char *b);
int strlen(const char *str);
int strnlen(const char *str, size_t max);
char *strchr(char *str, int c);
char *strrchr(const char *str, int c);
void strncpy(char *dest, const char *src, size_t max_len);
void strcpy(char *dest, const char *src);
void strcat(char *dest, const char *src);
void strncat(char *dest, const char *src, size_t count);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char *kstrdup(const char *s);
char *strndup(const char *s, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *cs, const char *ct);
char *strsep(char **s, const char *ct);
char *strstr(const char *haystack, const char *needle);
char *strnstr(const char *haystack, const char *needle, size_t len);
char *strtok(char *s, const char *ct);
char *strtok_r(char *s, const char *ct, char **last);

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

char *skip_spaces(const char *str);
char *strim(char *s);

/* Memory functions */
void *memcpy(void *d, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memset32(void *s, uint32_t val, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memscan(void *addr, int c, size_t size);

/* Advanced memory functions */
#ifdef CONFIG_STRING_ADVANCED
void *memrchr(const void *s, int c, size_t n);
void *memmem(const void *haystack, size_t hlen, const void *needle, size_t nlen);
void memswap(void *a, void *b, size_t n);
#endif

/* Cryptographic functions */
#ifdef CONFIG_STRING_CRYPTO
int memcmp_const_time(const void *s1, const void *s2, size_t n);
void explicit_bzero(void *s, size_t n);
#endif

/* Number conversions */
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base);
long long simple_strtoll(const char *cp, char **endp, unsigned int base);
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);
long simple_strtol(const char *cp, char **endp, unsigned int base);

uint64_t strtoul(const char *nptr, char **endptr, int base);
long strtol(const char *cp, char **endp, unsigned int base);

int kstrtoul(const char *s, unsigned int base, unsigned long *res);
int kstrtol(const char *s, unsigned int base, long *res);
int kstrtoull(const char *s, unsigned int base, unsigned long long *res);
int kstrtoll(const char *s, unsigned int base, long long *res);
int kstrtouint(const char *s, unsigned int base, unsigned int *res);
int kstrtoint(const char *s, unsigned int base, int *res);
int kstrtou8(const char *s, unsigned int base, uint8_t *res);
int kstrtos8(const char *s, unsigned int base, int8_t *res);
int kstrtou16(const char *s, unsigned int base, uint16_t *res);
int kstrtos16(const char *s, unsigned int base, int16_t *res);
int kstrtou32(const char *s, unsigned int base, uint32_t *res);
int kstrtos32(const char *s, unsigned int base, int32_t *res);

long long atoll(const char *s);
long atol(const char *s);
int atoi(const char *s);

/* Floating-point conversions */
#ifdef CONFIG_STRING_FLOAT
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
#endif

/* Pattern matching */
#ifdef CONFIG_STRING_PATTERN
int fnmatch(const char *pattern, const char *string, int flags);
int strverscmp(const char *s1, const char *s2);
#endif

/* Legacy/Internal */
void itoa(uint64_t n, char *buffer);
void htoa(uint64_t n, char *buffer);
bool find(const char* buff, const char* pattern);
bool is_word_boundary(char c);

/* Error handling */
int errno_to_str(char *restrict buff, const int err);

/* lib/cmdline.c */
int get_option(char **str, int *pint);
char *get_options(const char *str, int nints, int *ints);
unsigned long long memparse(const char *ptr, char **retptr);
bool parse_option_str(const char *str, const char *option);
char *next_arg(char *args, char **param, char **val);

/* arch/x86_64/lib/cmdline.c */
int cmdline_find_option_bool(const char *cmdline, const char *option);
int cmdline_find_option(const char *cmdline, const char *option, char *buffer,
      int bufsize);