#include <lib/string.h>

bool is_word_boundary(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\0';
}

bool find(const char* buff, const char* pattern) {
    if (buff == NULL || pattern == NULL || *pattern == '\0') {
        return false;
    }

    const size_t pattern_len = strlen(pattern);
    const char* ptr = buff;

    while ((ptr = strstr(ptr, pattern)) != NULL) {
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
        return (char *)haystack;
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
                return (char *)h_ptr;
            }
        }
    }

    return NULL;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
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
    return (unsigned char)*a - (unsigned char)*b;
}

int strlen(const char *str) {
    if (!str)
        return 0;
    int len = 0;
    while (str[len])
        len++;
    return len;
}

int strnlen(const char *str, const size_t max) {
    size_t len = 0;
    while (len < max && str[len])
        len++;
    return len;
}

const char *strchr(const char *str, int c) {
    char target = (char)c;

    // Loop until we hit the null terminator
    while (*str != '\0') {
        if (*str == target) {
            // Found the character, return a pointer to it
            return str;
        }
        str++;
    }

    // Special case: if the character we're looking for IS the null terminator,
    // the standard says we should return a pointer to it.
    if (target == '\0') {
        return str;
    }

    // If we get here, the character wasn't found
    return NULL;
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
    if (((uintptr_t)dest & 7) == 0 && ((uintptr_t)src & 7) == 0) {
        uint64_t *d64 = (uint64_t *)dest;
        const uint64_t *s64 = (const uint64_t *)src;

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
                char *d = (char *)d64;
                const char *s = (const char *)(s64 - 1);
                while ((*d++ = *s++))
                    ;
                return;
            }
            *d64++ = val;
        }
        *(char *)d64 = '\0';
    } else {
        // Original byte-by-byte copy for unaligned data
        while ((*dest++ = *src++))
            ;
    }
}

void strcat(char *dest, const char *src) {
    if (!dest || !src)
        return;
    while (*dest)
        dest++;
    strcpy(dest, src); // Reuse optimized strcpy
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
                return (char *)sc1;
        }
    }
    return NULL;
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

    if (sbegin == NULL)
        return NULL;

    end = strpbrk(sbegin, ct);
    if (end)
        *end++ = '\0';
    *s = end;

    return sbegin;
}

void *memset(void *restrict s, const int c, const size_t n) {
    unsigned char *p = s;
    const unsigned char byte = (unsigned char)c;
    for (size_t i = 0; i < n; ++i) {
        p[i] = byte;
    }
    return s;
}

void *memcpy(void *restrict dest, const void *restrict src, const size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void *memmove(void *dest, const void *src, const size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, const size_t n) {
    const unsigned char *p1 = s1;
    const unsigned char *p2 = s2;
    for (size_t i = 0; i < n; ++i) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}