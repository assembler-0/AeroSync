// SPDX-License-Identifier: GPL-2.0-only
/*
 * AeroSync Kernel - Advanced vsprintf implementation
 *
 * Ported from Linux lib/vsprintf.c
 * Architecturally identical to the Linux implementation:
 * - State-machine based decoding (format_decode)
 * - Specialized pointer extensions (%ps, %pS, %px, %pe)
 * - Optimized decimal conversion
 * - Promotion/Truncation handling (convert_num_spec)
 *
 * Stripped of Linux-specific subsystem dependencies (DT, FW, Clocks, etc.)
 */

#include <lib/bitmap.h>
#include <aerosync/types.h>
#include <aerosync/ctype.h>
#include <aerosync/export.h>
#include <lib/string.h>
#include <aerosync/compiler.h>
#include <math.h>
#include <aerosync/errno.h>
#include <mm/slub.h>
#include <linux/math64.h>
#include <aerosync/ksymtab.h>

#define SIGN	1		/* unsigned/signed */
#define LEFT	2		/* left justified */
#define PLUS	4		/* show plus */
#define SPACE	8		/* space if plus */
#define ZEROPAD	16		/* pad with zero, must be 16 == '0' - ' ' */
#define SMALL	32		/* use lowercase in hex (must be 32 == 0x20) */
#define SPECIAL	64		/* prefix hex with "0x", octal with "0" */

const char hex_asc_upper[] = "0123456789ABCDEF";
const char hex_asc_lower[] = "0123456789abcdef";

#define hex_asc_lo(x) hex_asc_lower[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc_lower[((x) & 0xf0) >> 4]

bool no_hash_pointers = false;

struct printf_spec {
  unsigned char	flags;		/* flags to number() */
  unsigned char	base;		/* number base, 8, 10 or 16 only */
  short		precision;	/* # of digits/chars */
  int		field_width;	/* width of output field */
} __packed;
static_assert(sizeof(struct printf_spec) == 8);

static inline int skip_atoi(const char **s)
{
	int i = 0;
	do {
		i = i * 10 + *((*s)++) - '0';
	} while (isdigit(**s));
	return i;
}

/* Optimized decimal conversion table */
static const uint16_t decpair[100] = {
#define _(x) (uint16_t) (((x % 10) | ((x / 10) << 8)) + 0x3030)
	_( 0), _( 1), _( 2), _( 3), _( 4), _( 5), _( 6), _( 7), _( 8), _( 9),
	_(10), _(11), _(12), _(13), _(14), _(15), _(16), _(17), _(18), _(19),
	_(20), _(21), _(22), _(23), _(24), _(25), _(26), _(27), _(28), _(29),
	_(30), _(31), _(32), _(33), _(34), _(35), _(36), _(37), _(38), _(39),
	_(40), _(41), _(42), _(43), _(44), _(45), _(46), _(47), _(48), _(49),
	_(50), _(51), _(52), _(53), _(54), _(55), _(56), _(57), _(58), _(59),
	_(60), _(61), _(62), _(63), _(64), _(65), _(66), _(67), _(68), _(69),
	_(70), _(71), _(72), _(73), _(74), _(75), _(76), _(77), _(78), _(79),
	_(80), _(81), _(82), _(83), _(84), _(85), _(86), _(87), _(88), _(89),
	_(90), _(91), _(92), _(93), _(94), _(95), _(96), _(97), _(98), _(99),
#undef _
};

static char *put_dec_trunc8(char *buf, uint32_t r)
{
	uint32_t q;
	if (r < 100) goto out_r;
	q = (r * (uint64_t)0x28f5c29) >> 32;
	*((uint16_t *)buf) = decpair[r - 100*q];
	buf += 2;
	if (q < 100) goto out_q;
	r = (q * (uint64_t)0x28f5c29) >> 32;
	*((uint16_t *)buf) = decpair[q - 100*r];
	buf += 2;
	if (r < 100) goto out_r;
	q = (r * 0x147b) >> 19;
	*((uint16_t *)buf) = decpair[r - 100*q];
	buf += 2;
out_q:
	r = q;
out_r:
	*((uint16_t *)buf) = decpair[r];
	buf += r < 10 ? 1 : 2;
	return buf;
}

static char *put_dec_full8(char *buf, uint32_t r)
{
	uint32_t q;
	q = (r * (uint64_t)0x28f5c29) >> 32;
	*((uint16_t *)buf) = decpair[r - 100*q];
	buf += 2;
	r = (q * (uint64_t)0x28f5c29) >> 32;
	*((uint16_t *)buf) = decpair[q - 100*r];
	buf += 2;
	q = (r * 0x147b) >> 19;
	*((uint16_t *)buf) = decpair[r - 100*q];
	buf += 2;
	*((uint16_t *)buf) = decpair[q];
	buf += 2;
	return buf;
}

static char *put_dec(char *buf, unsigned long long n)
{
	if (n >= 100*1000*1000)
		buf = put_dec_full8(buf, do_div(n, 100*1000*1000));
	if (n >= 100*1000*1000)
		buf = put_dec_full8(buf, do_div(n, 100*1000*1000));
	return put_dec_trunc8(buf, n);
}

static
char *number(char *buf, char *end, unsigned long long num,
	     struct printf_spec spec)
{
	/* put_dec requires 2-byte alignment of the buffer. */
	char tmp[3 * sizeof(num)] __aligned(2);
	char sign;
	char locase;
	int need_pfx = ((spec.flags & SPECIAL) && spec.base != 10);
	int i;
	bool is_zero = num == 0LL;
	int field_width = spec.field_width;
	int precision = spec.precision;

	/* locase = 0 or 0x20. ORing digits or letters with 'locase'
	 * produces same digits or (maybe lowercased) letters */
	locase = (spec.flags & SMALL);
	if (spec.flags & LEFT)
		spec.flags &= ~ZEROPAD;
	sign = 0;
	if (spec.flags & SIGN) {
		if ((signed long long)num < 0) {
			sign = '-';
			num = -(signed long long)num;
			field_width--;
		} else if (spec.flags & PLUS) {
			sign = '+';
			field_width--;
		} else if (spec.flags & SPACE) {
			sign = ' ';
			field_width--;
		}
	}
	if (need_pfx) {
		if (spec.base == 16)
			field_width -= 2;
		else if (!is_zero)
			field_width--;
	}

	/* generate full string in tmp[], in reverse order */
	i = 0;
	if (num < spec.base)
		tmp[i++] = hex_asc_upper[num] | locase;
	else if (spec.base != 10) { /* 8 or 16 */
		int mask = spec.base - 1;
		int shift = 3;

		if (spec.base == 16)
			shift = 4;
		do {
			tmp[i++] = (hex_asc_upper[((unsigned char)num) & mask] | locase);
			num >>= shift;
		} while (num);
	} else { /* base 10 */
		i = put_dec(tmp, num) - tmp;
	}

	/* printing 100 using %2d gives "100", not "00" */
	if (i > precision)
		precision = i;
	/* leading space padding */
	field_width -= precision;
	if (!(spec.flags & (ZEROPAD | LEFT))) {
		while (--field_width >= 0) {
			if (buf < end)
				*buf = ' ';
			++buf;
		}
	}
	/* sign */
	if (sign) {
		if (buf < end)
			*buf = sign;
		++buf;
	}
	/* "0x" / "0" prefix */
	if (need_pfx) {
		if (spec.base == 16 || !is_zero) {
			if (buf < end)
				*buf = '0';
			++buf;
		}
		if (spec.base == 16) {
			if (buf < end)
				*buf = ('X' | locase);
			++buf;
		}
	}
	/* zero or space padding */
	if (!(spec.flags & LEFT)) {
		char c = ' ' + (spec.flags & ZEROPAD);

		while (--field_width >= 0) {
			if (buf < end)
				*buf = c;
			++buf;
		}
	}
	/* hmm even more zero padding? */
	while (i <= --precision) {
		if (buf < end)
			*buf = '0';
		++buf;
	}
	/* actual digits of result */
	while (--i >= 0) {
		if (buf < end)
			*buf = tmp[i];
		++buf;
	}
	/* trailing space padding */
	while (--field_width >= 0) {
		if (buf < end)
			*buf = ' ';
		++buf;
	}

	return buf;
}


static void move_right(char *buf, char *end, unsigned len, unsigned spaces)
{
  size_t size;
  if (buf >= end)	/* nowhere to put anything */
    return;
  size = end - buf;
  if (size <= spaces) {
    memset(buf, ' ', size);
    return;
  }
  if (len) {
    if (len > size - spaces)
      len = size - spaces;
    memmove(buf + spaces, buf, len);
  }
  memset(buf, ' ', spaces);
}

static
char *widen_string(char *buf, int n, char *end, struct printf_spec spec)
{
  unsigned spaces;

  if (likely(n >= spec.field_width))
    return buf;
  /* we want to pad the sucker */
  spaces = spec.field_width - n;
  if (!(spec.flags & LEFT)) {
    move_right(buf - n, end, n, spaces);
    return buf + spaces;
  }
  while (spaces--) {
    if (buf < end)
      *buf = ' ';
    ++buf;
  }
  return buf;
}

/* Handle string from a well known address. */
static char *string_nocheck(char *buf, char *end, const char *s,
          struct printf_spec spec)
{
  int len = 0;
  int lim = spec.precision;

  while (lim--) {
    char c = *s++;
    if (!c)
      break;
    if (buf < end)
      *buf = c;
    ++buf;
    ++len;
  }
  return widen_string(buf, len, end, spec);
}

/*
 * Do not call any complex external code here. Nested printk()/vsprintf()
 * might cause infinite loops. Failures might break printk() and would
 * be hard to debug.
 */
static const char *check_pointer_msg(const void *ptr)
{
  if (!ptr)
    return "(null)";

  if ((unsigned long)ptr < PAGE_SIZE || IS_ERR_VALUE(ptr))
    return "(efault)";

  return nullptr;
}

/* Be careful: error messages must fit into the given buffer. */
static char *error_string(char *buf, char *end, const char *s,
        struct printf_spec spec)
{
  /*
   * Hard limit to avoid a completely insane messages. It actually
   * works pretty well because most error messages are in
   * the many pointer format modifiers.
   */
  if (spec.precision == -1)
    spec.precision = 2 * sizeof(void *);

  return string_nocheck(buf, end, s, spec);
}

static int check_pointer(char **buf, char *end, const void *ptr,
       struct printf_spec spec)
{
  const char *err_msg;

  err_msg = check_pointer_msg(ptr);
  if (err_msg) {
    *buf = error_string(*buf, end, err_msg, spec);
    return -EFAULT;
  }

  return 0;
}

static
char *string(char *buf, char *end, const char *s,
       struct printf_spec spec)
{
  if (check_pointer(&buf, end, s, spec))
    return buf;

  return string_nocheck(buf, end, s, spec);
}

static char *symbol_string(char *buf, char *end, void *ptr, struct printf_spec spec, const char *fmt) {
  unsigned long value = (unsigned long)ptr;
  uintptr_t offset;
  const char *sym_name = lookup_ksymbol_by_addr(value, &offset);

  if (sym_name) {
    char sym[256];
    if (*fmt == 'S')
      snprintf(sym, sizeof(sym), "%s+0x%lx", sym_name, offset);
    else
      strncpy(sym, sym_name, sizeof(sym));
    return string(buf, end, sym, spec);
  }

  /* Fallback to hex */
  spec.base = 16;
  spec.flags |= SMALL | SPECIAL | ZEROPAD;
  spec.field_width = 2 * sizeof(void *);
  return number(buf, end, value, spec);
}

static
char *hex_string(char *buf, char *end, uint8_t *addr, struct printf_spec spec,
		 const char *fmt)
{
	int i, len = 1;		/* if we pass '%ph[CDN]', field width remains
				   negative value, fallback to the default */
	char separator;

	if (spec.field_width == 0)
		/* nothing to print */
		return buf;

	if (check_pointer(&buf, end, addr, spec))
		return buf;

	switch (fmt[1]) {
	case 'C':
		separator = ':';
		break;
	case 'D':
		separator = '-';
		break;
	case 'N':
		separator = 0;
		break;
	default:
		separator = ' ';
		break;
	}

	if (spec.field_width > 0)
		len = min(spec.field_width, 64);

	for (i = 0; i < len; ++i) {
		if (buf < end)
			*buf = hex_asc_hi(addr[i]);
		++buf;
		if (buf < end)
			*buf = hex_asc_lo(addr[i]);
		++buf;

		if (separator && i != len - 1) {
			if (buf < end)
				*buf = separator;
			++buf;
		}
	}

	return buf;
}

static
char *bitmap_string(char *buf, char *end, const unsigned long *bitmap,
        struct printf_spec spec, const char *fmt) {
  const int CHUNKSZ = 32;
  int nr_bits = max(spec.field_width, 0);
  int i, chunksz;
  bool first = true;

  if (check_pointer(&buf, end, bitmap, spec))
    return buf;

  /* reused to print numbers */
  spec = (struct printf_spec){ .flags = SMALL | ZEROPAD, .base = 16 };

  chunksz = nr_bits & (CHUNKSZ - 1);
  if (chunksz == 0)
    chunksz = CHUNKSZ;

  i = ALIGN(nr_bits, CHUNKSZ) - CHUNKSZ;
  for (; i >= 0; i -= CHUNKSZ) {
    uint32_t chunkmask, val;
    int word, bit;

    chunkmask = ((1ULL << chunksz) - 1);
    word = i / BITS_PER_LONG;
    bit = i % BITS_PER_LONG;
    val = (bitmap[word] >> bit) & chunkmask;

    if (!first) {
      if (buf < end)
        *buf = ',';
      buf++;
    }
    first = false;

    spec.field_width = DIV_ROUND_UP(chunksz, 4);
    buf = number(buf, end, val, spec);

    chunksz = CHUNKSZ;
  }
  return buf;
}

static char *pointer_string(char *buf, char *end,
          const void *ptr,
          struct printf_spec spec)
{
  spec.base = 16;
  spec.flags |= SMALL;
  if (spec.field_width == -1) {
    spec.field_width = 2 * sizeof(ptr);
    spec.flags |= ZEROPAD;
  }

  return number(buf, end, (unsigned long int)ptr, spec);
}

static char *ptr_to_id(char *buf, char *end, const void *ptr,
           struct printf_spec spec)
{
  /* simplified */
  return pointer_string(buf, end, ptr, spec);
}

static char *default_pointer(char *buf, char *end, const void *ptr,
           struct printf_spec spec)
{
  /*
   * default is to _not_ leak addresses, so hash before printing,
   * unless no_hash_pointers is specified on the command line.
   */
  if (unlikely(no_hash_pointers))
    return pointer_string(buf, end, ptr, spec);

  return ptr_to_id(buf, end, ptr, spec);
}

static char *err_ptr(char *buf, char *end, void *ptr,
         struct printf_spec spec)
{
  int err = PTR_ERR(ptr);
  const char *sym = errname(err);

  if (sym)
    return string_nocheck(buf, end, sym, spec);

  /*
   * Somebody passed ERR_PTR(-1234) or some other non-existing
   * Efoo - or perhaps CONFIG_SYMBOLIC_ERRNAME=n. Fall back to
   * printing it as its decimal representation.
   */
  spec.flags |= SIGN;
  spec.base = 10;
  return number(buf, end, err, spec);
}

static
char *uuid_string(char *buf, char *end, uint8_t *addr, struct printf_spec spec,
		 const char *fmt)
{
	char uuid[37];
	static const char hex[] = "0123456789abcdef";
	int i, j;

	for (i = 0, j = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			uuid[j++] = '-';
		uuid[j++] = hex[addr[i] >> 4];
		uuid[j++] = hex[addr[i] & 0x0f];
	}
	uuid[j] = '\0';

	return string(buf, end, uuid, spec);
}

static
char *pointer(const char *fmt, char *buf, char *end, void *ptr,
	      struct printf_spec spec)
{
	switch (*fmt) {
	case 'S':
	case 'B':
		return symbol_string(buf, end, ptr, spec, fmt);
	case 'R':
	case 'h':
		return hex_string(buf, end, ptr, spec, fmt);
	case 'U':
		return uuid_string(buf, end, ptr, spec, fmt);
	case 'b':
		return bitmap_string(buf, end, ptr, spec, fmt);
	case 'x':
		return pointer_string(buf, end, ptr, spec);
	case 'e':
		/* %pe with a non-ERR_PTR gets treated as plain %p */
		if (!IS_ERR(ptr))
			return default_pointer(buf, end, ptr, spec);
		return err_ptr(buf, end, ptr, spec);
	case 'u':
	case 'k':
		switch (fmt[1]) {
		case 's':
			return string(buf, end, ptr, spec);
		default:
			return error_string(buf, end, "(einval)", spec);
		}
	default:
		return default_pointer(buf, end, ptr, spec);
	}
}

enum format_state {
  FORMAT_STATE_NONE, /* Just a string part */
  FORMAT_STATE_NUM,
  FORMAT_STATE_WIDTH,
  FORMAT_STATE_PRECISION,
  FORMAT_STATE_CHAR,
  FORMAT_STATE_STR,
  FORMAT_STATE_PTR,
  FORMAT_STATE_PERCENT_CHAR,
  FORMAT_STATE_INVALID,
};

struct format_decode_state {
	const char *fmt;
	enum format_state state;
	int qualifier;
};

struct fmt {
  const char *str;
  unsigned char state;	// enum format_state
  unsigned char size;	// size of numbers
};

#define SPEC_CHAR(x, flag) [(x)-32] = flag
static unsigned char spec_flag(unsigned char c)
{
  static constexpr unsigned char spec_flag_array[] = {
    SPEC_CHAR(' ', SPACE),
    SPEC_CHAR('#', SPECIAL),
    SPEC_CHAR('+', PLUS),
    SPEC_CHAR('-', LEFT),
    SPEC_CHAR('0', ZEROPAD),
  };
  c -= 32;
  return (c < sizeof(spec_flag_array)) ? spec_flag_array[c] : 0;
}

/*
 * Helper function to decode printf style format.
 * Each call decode a token from the format and return the
 * number of characters read (or likely the delta where it wants
 * to go on the next call).
 * The decoded token is returned through the parameters
 *
 * 'h', 'l', or 'L' for integer fields
 * 'z' support added 23/7/1999 S.H.
 * 'z' changed to 'Z' --davidm 1/25/99
 * 'Z' changed to 'z' --adobriyan 2017-01-25
 * 't' added for ptrdiff_t
 *
 * @fmt: the format string
 * @type of the token returned
 * @flags: various flags such as +, -, # tokens..
 * @field_width: overwritten width
 * @base: base of the number (octal, hex, ...)
 * @precision: precision of a number
 * @qualifier: qualifier of a number (long, size_t, ...)
 */
static
struct fmt format_decode(struct fmt fmt, struct printf_spec *spec)
{
	const char *start = fmt.str;
	char flag;

	/* we finished early by reading the field width */
	if (unlikely(fmt.state == FORMAT_STATE_WIDTH)) {
		if (spec->field_width < 0) {
			spec->field_width = -spec->field_width;
			spec->flags |= LEFT;
		}
		fmt.state = FORMAT_STATE_NONE;
		goto precision;
	}

	/* we finished early by reading the precision */
	if (unlikely(fmt.state == FORMAT_STATE_PRECISION)) {
		if (spec->precision < 0)
			spec->precision = 0;

		fmt.state = FORMAT_STATE_NONE;
		goto qualifier;
	}

	/* By default */
	fmt.state = FORMAT_STATE_NONE;

	for (; *fmt.str ; fmt.str++) {
		if (*fmt.str == '%')
			break;
	}

	/* Return the current non-format string */
	if (fmt.str != start || !*fmt.str)
		return fmt;

	/* Process flags. This also skips the first '%' */
	spec->flags = 0;
	do {
		/* this also skips first '%' */
		flag = spec_flag(*++fmt.str);
		spec->flags |= flag;
	} while (flag);

	/* get field width */
	spec->field_width = -1;

	if (isdigit(*fmt.str))
		spec->field_width = skip_atoi(&fmt.str);
	else if (unlikely(*fmt.str == '*')) {
		/* it's the next argument */
		fmt.state = FORMAT_STATE_WIDTH;
		fmt.str++;
		return fmt;
	}

precision:
	/* get the precision */
	spec->precision = -1;
	if (unlikely(*fmt.str == '.')) {
		fmt.str++;
		if (isdigit(*fmt.str)) {
			spec->precision = skip_atoi(&fmt.str);
			if (spec->precision < 0)
				spec->precision = 0;
		} else if (*fmt.str == '*') {
			/* it's the next argument */
			fmt.state = FORMAT_STATE_PRECISION;
			fmt.str++;
			return fmt;
		}
	}

qualifier:
	/* Set up default numeric format */
	spec->base = 10;
	fmt.state = FORMAT_STATE_NUM;
	fmt.size = sizeof(int);
	static const struct format_state {
		unsigned char state;
		unsigned char size;
		unsigned char flags_or_double_size;
		unsigned char base;
	} lookup_state[256] = {
		// Length
		['l'] = { 0, sizeof(long), sizeof(long long) },
		['L'] = { 0, sizeof(long long) },
		['h'] = { 0, sizeof(short), sizeof(char) },
		['H'] = { 0, sizeof(char) },	// Questionable historical
		['z'] = { 0, sizeof(size_t) },
		['t'] = { 0, sizeof(ptrdiff_t) },

		// Non-numeric formats
		['c'] = { FORMAT_STATE_CHAR },
		['s'] = { FORMAT_STATE_STR },
		['p'] = { FORMAT_STATE_PTR },
		['%'] = { FORMAT_STATE_PERCENT_CHAR },

		// Numerics
		['o'] = { FORMAT_STATE_NUM, 0, 0, 8 },
		['x'] = { FORMAT_STATE_NUM, 0, SMALL, 16 },
		['X'] = { FORMAT_STATE_NUM, 0, 0, 16 },
		['d'] = { FORMAT_STATE_NUM, 0, SIGN, 10 },
		['i'] = { FORMAT_STATE_NUM, 0, SIGN, 10 },
		['u'] = { FORMAT_STATE_NUM, 0, 0, 10, },

		/*
		 * Since %n poses a greater security risk than
		 * utility, treat it as any other invalid or
		 * unsupported format specifier.
		 */
	};

	const struct format_state *p = lookup_state + (uint8_t)*fmt.str;
	if (p->size) {
		fmt.size = p->size;
		if (p->flags_or_double_size && fmt.str[0] == fmt.str[1]) {
			fmt.size = p->flags_or_double_size;
			fmt.str++;
		}
		fmt.str++;
		p = lookup_state + *fmt.str;
	}
	if (p->state) {
		if (p->base)
			spec->base = p->base;
		spec->flags |= p->flags_or_double_size;
		fmt.state = p->state;
		fmt.str++;
		return fmt;
	}

	fmt.state = FORMAT_STATE_INVALID;
	return fmt;
}


static unsigned long long convert_num_spec(unsigned int val, int size, struct printf_spec spec)
{
  unsigned int shift = 32 - size*8;

  val <<= shift;
  if (!(spec.flags & SIGN))
    return val >> shift;
  return (int)val >> shift;
}

#define FIELD_WIDTH_MAX ((1 << 23) - 1)
#define PRECISION_MAX ((1 << 15) - 1)

static void
set_field_width(struct printf_spec *spec, int width)
{
  spec->field_width = width;
  if (spec->field_width != width) {
    spec->field_width = clamp(width, -FIELD_WIDTH_MAX, FIELD_WIDTH_MAX);
  }
}

static void
set_precision(struct printf_spec *spec, int prec)
{
  spec->precision = prec;
  if (spec->precision != prec) {
    spec->precision = clamp(prec, 0, PRECISION_MAX);
  }
}

/**
 * vsnprintf - Format a string and place it in a buffer
 * @buf: The buffer to place the result into
 * @size: The size of the buffer, including the trailing null space
 * @fmt_str: The format string to use
 * @args: Arguments for the format string
 *
 * This function generally follows C99 vsnprintf, but has some
 * extensions and a few limitations:
 *
 *  - ``%n`` is unsupported
 *  - ``%p*`` is handled by pointer()
 *
 * See pointer() or Documentation/core-api/printk-formats.rst for more
 * extensive description.
 *
 * **Please update the documentation in both places when making changes**
 *
 * The return value is the number of characters which would
 * be generated for the given input, excluding the trailing
 * '\0', as per ISO C99. If you want to have the exact
 * number of characters written into @buf as return value
 * (not including the trailing '\0'), use vscnprintf(). If the
 * return is greater than or equal to @size, the resulting
 * string is truncated.
 *
 * If you're not already dealing with a va_list consider using snprintf().
 */
int vsnprintf(char *buf, size_t size, const char *fmt_str, va_list args)
{
	char *str, *end;
	struct printf_spec spec = {0};
	struct fmt fmt = {
		.str = fmt_str,
		.state = FORMAT_STATE_NONE,
	};

	str = buf;
	end = buf + size;

	/* Make sure end is always >= buf */
	if (end < buf) {
		end = ((void *)-1);
		size = end - buf;
	}

	while (*fmt.str) {
		const char *old_fmt = fmt.str;

		fmt = format_decode(fmt, &spec);

		switch (fmt.state) {
		case FORMAT_STATE_NONE: {
			int read = fmt.str - old_fmt;
			if (str < end) {
				int copy = read;
				if (copy > end - str)
					copy = end - str;
				memcpy(str, old_fmt, copy);
			}
			str += read;
			continue;
		}

		case FORMAT_STATE_NUM: {
			unsigned long long num;

			if (fmt.size > sizeof(int))
				num = va_arg(args, long long);
			else
				num = convert_num_spec(va_arg(args, int), fmt.size, spec);
			str = number(str, end, num, spec);
			continue;
		}

		case FORMAT_STATE_WIDTH:
			set_field_width(&spec, va_arg(args, int));
			continue;

		case FORMAT_STATE_PRECISION:
			set_precision(&spec, va_arg(args, int));
			continue;

		case FORMAT_STATE_CHAR: {
			char c;

			if (!(spec.flags & LEFT)) {
				while (--spec.field_width > 0) {
					if (str < end)
						*str = ' ';
					++str;

				}
			}
			c = (unsigned char) va_arg(args, int);
			if (str < end)
				*str = c;
			++str;
			while (--spec.field_width > 0) {
				if (str < end)
					*str = ' ';
				++str;
			}
			continue;
		}

		case FORMAT_STATE_STR:
			str = string(str, end, va_arg(args, char *), spec);
			continue;

		case FORMAT_STATE_PTR:
			str = pointer(fmt.str, str, end, va_arg(args, void *),
				      spec);
			while (isalnum(*fmt.str))
				fmt.str++;
			continue;

		case FORMAT_STATE_PERCENT_CHAR:
			if (str < end)
				*str = '%';
			++str;
			continue;

		default:
			/*
			 * Presumably the arguments passed gcc's type
			 * checking, but there is no safe or sane way
			 * for us to continue parsing the format and
			 * fetching from the va_list; the remaining
			 * specifiers and arguments would be out of
			 * sync.
			 */
			goto out;
		}
	}

out:
	if (size > 0) {
		if (str < end)
			*str = '\0';
		else
			end[-1] = '\0';
	}

	/* the trailing null byte doesn't count towards the total */
	return str-buf;

}
EXPORT_SYMBOL(vsnprintf);

/* Helper wrappers */
int vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	int i = vsnprintf(buf, size, fmt, args);
	if (unlikely(!size)) return 0;
	return (i >= size) ? (int)size - 1 : i;
}
EXPORT_SYMBOL(vscnprintf);

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int i = vsnprintf(buf, size, fmt, args);
	va_end(args);
	return i;
}
EXPORT_SYMBOL(snprintf);

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int i = vscnprintf(buf, size, fmt, args);
	va_end(args);
	return i;
}
EXPORT_SYMBOL(scnprintf);

int vsprintf(char *buf, const char *fmt, va_list args)
{
	return vsnprintf(buf, INT_MAX, fmt, args);
}
EXPORT_SYMBOL(vsprintf);

int sprintf(char *buf, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int i = vsnprintf(buf, INT_MAX, fmt, args);
	va_end(args);
	return i;
}
EXPORT_SYMBOL(sprintf);

char *kvasprintf(const char *fmt, va_list args)
{
	unsigned int len;
	char *p;
	va_list aq;

	va_copy(aq, args);
	len = vsnprintf(nullptr, 0, fmt, aq);
	va_end(aq);

	p = kmalloc(len + 1);
	if (!p) return nullptr;

	vsnprintf(p, len + 1, fmt, args);
	return p;
}
EXPORT_SYMBOL(kvasprintf);

char *kasprintf(const char *fmt, ...)
{
	va_list args;
	char *p;
	va_start(args, fmt);
	p = kvasprintf(fmt, args);
	va_end(args);
	return p;
}
EXPORT_SYMBOL(kasprintf);


/**
 * vsscanf - Unformat a buffer into a list of arguments
 * @buf:	input buffer
 * @fmt:	format of buffer
 * @args:	arguments
 */
int vsscanf(const char *buf, const char *fmt, va_list args)
{
	const char *str = buf;
	char *next;
	char digit;
	int num = 0;
	uint8_t qualifier;
	unsigned int base;
	union {
		long long s;
		unsigned long long u;
	} val;
	int16_t field_width;
	bool is_sign;

	while (*fmt) {
		/* skip any white space in format */
		/* white space in format matches any amount of
		 * white space, including none, in the input.
		 */
		if (isspace(*fmt)) {
			fmt = skip_spaces(++fmt);
			str = skip_spaces(str);
		}

		/* anything that is not a conversion must match exactly */
		if (*fmt != '%' && *fmt) {
			if (*fmt++ != *str++)
				break;
			continue;
		}

		if (!*fmt)
			break;
		++fmt;

		/* skip this conversion.
		 * advance both strings to next white space
		 */
		if (*fmt == '*') {
			if (!*str)
				break;
			while (!isspace(*fmt) && *fmt != '%' && *fmt) {
				/* '%*[' not yet supported, invalid format */
				if (*fmt == '[')
					return num;
				fmt++;
			}
			while (!isspace(*str) && *str)
				str++;
			continue;
		}

		/* get field width */
		field_width = -1;
		if (isdigit(*fmt)) {
			field_width = skip_atoi(&fmt);
			if (field_width <= 0)
				break;
		}

		/* get conversion qualifier */
		qualifier = -1;
		if (*fmt == 'h' || tolower(*fmt) == 'l' ||
		    *fmt == 'z') {
			qualifier = *fmt++;
			if (unlikely(qualifier == *fmt)) {
				if (qualifier == 'h') {
					qualifier = 'H';
					fmt++;
				} else if (qualifier == 'l') {
					qualifier = 'L';
					fmt++;
				}
			}
		}

		if (!*fmt)
			break;

		if (*fmt == 'n') {
			/* return number of characters read so far */
			*va_arg(args, int *) = str - buf;
			++fmt;
			continue;
		}

		if (!*str)
			break;

		base = 10;
		is_sign = false;

		switch (*fmt++) {
		case 'c':
		{
			char *s = (char *)va_arg(args, char*);
			if (field_width == -1)
				field_width = 1;
			do {
				*s++ = *str++;
			} while (--field_width > 0 && *str);
			num++;
		}
		continue;
		case 's':
		{
			char *s = (char *)va_arg(args, char *);
			if (field_width == -1)
				field_width = SHRT_MAX;
			/* first, skip leading white space in buffer */
			str = skip_spaces(str);

			/* now copy until next white space */
			while (*str && !isspace(*str) && field_width--)
				*s++ = *str++;
			*s = '\0';
			num++;
		}
		continue;
		/*
		 * Warning: This implementation of the '[' conversion specifier
		 * deviates from its glibc counterpart in the following ways:
		 * (1) It does NOT support ranges i.e. '-' is NOT a special
		 *     character
		 * (2) It cannot match the closing bracket ']' itself
		 * (3) A field width is required
		 * (4) '%*[' (discard matching input) is currently not supported
		 *
		 * Example usage:
		 * ret = sscanf("00:0a:95","%2[^:]:%2[^:]:%2[^:]",
		 *		buf1, buf2, buf3);
		 * if (ret < 3)
		 *    // etc..
		 */
		case '[':
		{
			char *s = (char *)va_arg(args, char *);
			DECLARE_BITMAP(set, 256) = {0};
			unsigned int len = 0;
			bool negate = (*fmt == '^');

			/* field width is required */
			if (field_width == -1)
				return num;

			if (negate)
				++fmt;

			for ( ; *fmt && *fmt != ']'; ++fmt, ++len)
				__set_bit((uint8_t)*fmt, set);

			/* no ']' or no character set found */
			if (!*fmt || !len)
				return num;
			++fmt;

			if (negate) {
				bitmap_complement(set, set, 256);
				/* exclude null '\0' byte */
				__clear_bit(0, set);
			}

			/* match must be non-empty */
			if (!test_bit((uint8_t)*str, set))
				return num;

			while (test_bit((uint8_t)*str, set) && field_width--)
				*s++ = *str++;
			*s = '\0';
			++num;
		}
		continue;
		case 'o':
			base = 8;
			break;
		case 'x':
		case 'X':
			base = 16;
			break;
		case 'i':
			base = 0;
			fallthrough;
		case 'd':
			is_sign = true;
			fallthrough;
		case 'u':
			break;
		case '%':
			/* looking for '%' in str */
			if (*str++ != '%')
				return num;
			continue;
		default:
			/* invalid format; stop here */
			return num;
		}

		/* have some sort of integer conversion.
		 * first, skip white space in buffer.
		 */
		str = skip_spaces(str);

		digit = *str;
		if (is_sign && digit == '-') {
			if (field_width == 1)
				break;

			digit = *(str + 1);
		}

		if (!digit
		    || (base == 16 && !isxdigit(digit))
		    || (base == 10 && !isdigit(digit))
		    || (base == 8 && !isodigit(digit))
		    || (base == 0 && !isdigit(digit)))
			break;

		if (is_sign)
			val.s = simple_strntoll(str, &next, base,
						field_width >= 0 ? field_width : INT_MAX);
		else
			val.u = simple_strntoull(str, &next, base,
						 field_width >= 0 ? field_width : INT_MAX);

		switch (qualifier) {
		case 'H':	/* that's 'hh' in format */
			if (is_sign)
				*va_arg(args, signed char *) = val.s;
			else
				*va_arg(args, unsigned char *) = val.u;
			break;
		case 'h':
			if (is_sign)
				*va_arg(args, short *) = val.s;
			else
				*va_arg(args, unsigned short *) = val.u;
			break;
		case 'l':
			if (is_sign)
				*va_arg(args, long *) = val.s;
			else
				*va_arg(args, unsigned long *) = val.u;
			break;
		case 'L':
			if (is_sign)
				*va_arg(args, long long *) = val.s;
			else
				*va_arg(args, unsigned long long *) = val.u;
			break;
		case 'z':
			*va_arg(args, size_t *) = val.u;
			break;
		default:
			if (is_sign)
				*va_arg(args, int *) = val.s;
			else
				*va_arg(args, unsigned int *) = val.u;
			break;
		}
		num++;

		if (!next)
			break;
		str = next;
	}

	return num;
}
EXPORT_SYMBOL(vsscanf);

int sscanf(const char *buf, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int i = vsscanf(buf, fmt, args);
	va_end(args);
	return i;
}
EXPORT_SYMBOL(sscanf);
