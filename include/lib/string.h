#pragma once

#include <kernel/types.h>

int strncmp(const char *a, const char *b, size_t n);
int strcmp(const char *a, const char *b);
int strlen(const char *str);
int strnlen(const char *str, size_t max);
char *strchr(char *str, int c);
char *strrchr(const char *str, int c);
void strncpy(char *dest, const char *src, size_t max_len);
void strcpy(char *dest, const char *src);
void strcat(char *dest, const char *src);
void itoa(uint64_t n, char *buffer);
void htoa(uint64_t n, char *buffer);
size_t strspn(const char *s, const char *accept);
char *strpbrk(const char *cs, const char *ct);
char *strsep(char **s, const char *ct);
char *strstr(const char *haystack, const char *needle);
bool find(const char* buff, const char* pattern);
bool is_word_boundary(char c);

void *memcpy(void *d, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memset32(void *s, uint32_t val, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
uint64_t strtoul(const char *nptr, char **endptr, int base);
