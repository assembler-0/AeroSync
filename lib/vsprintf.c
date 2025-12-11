/*
 *  linux/kernel/vsprintf.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/* vsprintf.c -- Lars Wirzenius & Linus Torvalds. */
/*
 * Wirzenius wrote this portably, Torvalds fucked it up :-)
 */

/**
 *@brief vsnprintf and formatting wrappers implementation modified for x86_64
 *@date 02/09/25 by assembler-0
 */

#include <lib/string.h>
#include <kernel/types.h>
#include <kernel/ctype.h>

unsigned long simple_strtoul(const char *cp,char **endp,unsigned int base)
{
	unsigned long result = 0,value;

	if (!base) {
		base = 10;
		if (*cp == '0') {
			base = 8;
			cp++;
			if ((*cp == 'x') && isxdigit(cp[1])) {
				cp++;
				base = 16;
			}
		}
	}
	while (isxdigit(*cp) && (value = isdigit(*cp) ? *cp-'0' : (islower(*cp)
	    ? toupper(*cp) : *cp)-'A'+10) < base) {
		result = result*base + value;
		cp++;
	}
	if (endp)
		*endp = (char *)cp;
	return result;
}

/* we use this so that we can do without the ctype library */
#define is_digit(c)	((c) >= '0' && (c) <= '9')

static int skip_atoi(const char **s)
{
	int i=0;

	while (is_digit(**s))
		i = i*10 + *((*s)++) - '0';
	return i;
}

#define ZEROPAD	1		/* pad with zero */
#define SIGN	2		/* unsigned/signed long */
#define PLUS	4		/* show plus */
#define SPACE	8		/* space if plus */
#define LEFT	16		/* left justified */
#define SPECIAL	32		/* 0x */
#define SMALL	64		/* use 'abcdef' instead of 'ABCDEF' */

#if defined(__i386__)
#define do_div(n, base) ({ \
unsigned int __res; \
__asm__("divl %4" : "=a"(n), "=d"(__res) : "0"(n), "1"(0), "r"(base)); \
__res; })
#else
#define do_div(n, base) ({ \
unsigned long __rem = (unsigned long)(n) % (unsigned long)(base); \
(n) = (unsigned long)(n) / (unsigned long)(base); \
(unsigned int)__rem; })
#endif

static void number_to_str(char* buf, size_t buf_size, int* pos, unsigned long long num, int base, int flags)
{
	char tmp[24];
	const char *digits = (flags & SMALL) ? "0123456789abcdefghijklmnopqrstuvwxyz" : "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int i = 0;

	if (num == 0) {
		tmp[i++] = '0';
	} else {
		while (num && i < 23) {
			tmp[i++] = digits[num % base];
			num /= base;
		}
	}

	while (i > 0 && *pos < (int)buf_size - 1) {
		buf[(*pos)++] = tmp[--i];
	}
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	int pos = 0;
	const char *s;
	int len, i;
	int flags;

	if (!buf || size == 0) return 0;

	for (; *fmt && pos < (int)size - 1; ++fmt) {
		if (*fmt != '%') {
			buf[pos++] = *fmt;
			continue;
		}

		++fmt;
		flags = 0;

		/* Skip flags */
		while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0') {
			if (*fmt == 'x') flags |= SMALL;
			++fmt;
		}

		/* Skip field width */
		while (is_digit(*fmt)) ++fmt;

		/* Skip precision */
		if (*fmt == '.') {
			++fmt;
			while (is_digit(*fmt)) ++fmt;
		}

		/* Parse length modifier */
		int is_long = 0, is_longlong = 0, is_size_t = 0, is_ptrdiff = 0;
		if (*fmt == 'l') {
			++fmt;
			is_long = 1;
			if (*fmt == 'l') {
				++fmt;
				is_longlong = 1;
				is_long = 0;
			}
		} else if (*fmt == 'z') {
			++fmt;
			is_size_t = 1;
		} else if (*fmt == 't') {
			++fmt;
			is_ptrdiff = 1;
		} else if (*fmt == 'h' || *fmt == 'L') {
			++fmt;
		}

		switch (*fmt) {
		case 'c':
			if (pos < (int)size - 1) buf[pos++] = (char)va_arg(args, int);
			break;

		case 's':
			s = va_arg(args, char *);
			if (!s) s = "(null)";
			len = strlen(s);
			for (i = 0; i < len && pos < (int)size - 1; i++) {
				buf[pos++] = s[i];
			}
			break;

		case 'd':
		case 'i': {
			long long snum;
			if (is_longlong) snum = va_arg(args, long long);
			else if (is_long) snum = va_arg(args, long);
			else if (is_size_t) snum = va_arg(args, ssize_t);
			else if (is_ptrdiff) snum = va_arg(args, ptrdiff_t);
			else snum = va_arg(args, int);

			if (snum < 0 && pos < (int)size - 1) {
				buf[pos++] = '-';
				snum = -snum;
			}
			number_to_str(buf, size, &pos, (unsigned long long)snum, 10, 0);
			break;
		}

		case 'u': {
			unsigned long long unum;
			if (is_longlong) unum = va_arg(args, unsigned long long);
			else if (is_long) unum = va_arg(args, unsigned long);
			else if (is_size_t) unum = va_arg(args, size_t);
			else if (is_ptrdiff) unum = va_arg(args, ptrdiff_t);
			else unum = va_arg(args, unsigned int);
			number_to_str(buf, size, &pos, unum, 10, 0);
			break;
		}

		case 'x':
			flags |= SMALL;
		case 'X': {
			unsigned long long unum;
			if (is_longlong) unum = va_arg(args, unsigned long long);
			else if (is_long) unum = va_arg(args, unsigned long);
			else if (is_size_t) unum = va_arg(args, size_t);
			else if (is_ptrdiff) unum = va_arg(args, ptrdiff_t);
			else unum = va_arg(args, unsigned int);
			number_to_str(buf, size, &pos, unum, 16, flags);
			break;
		}

		case 'o': {
			unsigned long long unum;
			if (is_longlong) unum = va_arg(args, unsigned long long);
			else if (is_long) unum = va_arg(args, unsigned long);
			else if (is_size_t) unum = va_arg(args, size_t);
			else if (is_ptrdiff) unum = va_arg(args, ptrdiff_t);
			else unum = va_arg(args, unsigned int);
			number_to_str(buf, size, &pos, unum, 8, 0);
			break;
		}

		case 'p':
			if (pos < (int)size - 1) buf[pos++] = '0';
			if (pos < (int)size - 1) buf[pos++] = 'x';
			number_to_str(buf, size, &pos, (unsigned long long)va_arg(args, void *), 16, SMALL);
			break;

		case 'n': {
			/* Store number of characters written so far */
			int *ip = va_arg(args, int *);
			if (ip) *ip = pos;
			break;
		}

		case 'b': {
			/* Binary format (non-standard but useful) */
			unsigned long long unum;
			if (is_longlong) unum = va_arg(args, unsigned long long);
			else if (is_long) unum = va_arg(args, unsigned long);
			else if (is_size_t) unum = va_arg(args, size_t);
			else if (is_ptrdiff) unum = va_arg(args, ptrdiff_t);
			else unum = va_arg(args, unsigned int);
			number_to_str(buf, size, &pos, unum, 2, 0);
			break;
		}

		case '%':
			if (pos < (int)size - 1) buf[pos++] = '%';
			break;

		default:
			if (pos < (int)size - 1) buf[pos++] = '%';
			if (*fmt && pos < (int)size - 1) buf[pos++] = *fmt;
			break;
		}
	}

	buf[pos] = '\0';
	return pos;
}

int vsprintf(char *buf, const char *fmt, va_list args)
{
	return vsnprintf(buf, 0x7FFFFFFF, fmt, args);
}

int snprintf(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}