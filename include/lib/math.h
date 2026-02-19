#pragma once

#include <aerosync/types.h>
#include <linux/minmax.h>
#include <linux/log2.h>

/**
 * DIV_ROUND_UP_POW2 - divide and round up
 * @n: numerator
 * @d: denominator (must be a power of 2)
 *
 * Divides @n by @d and rounds up to next multiple of @d (which must be a power
 * of 2). Avoids integer overflows that may occur with __KERNEL_DIV_ROUND_UP().
 * Performance is roughly equivalent to __KERNEL_DIV_ROUND_UP().
 */
#define __KERNEL_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define DIV_ROUND_UP_POW2(n, d) \
((n) / (d) + !!((n) & ((d) - 1)))

#define DIV_ROUND_UP __KERNEL_DIV_ROUND_UP

#define DIV_ROUND_DOWN_ULL(ll, d) \
({ unsigned long long _tmp = (ll); do_div(_tmp, d); _tmp; })

#define DIV_ROUND_UP_ULL(ll, d) \
DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))

/* Mathematical constants */
#define M_E         2.71828182845904523536
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.434294481903251827651
#define M_LN2       0.693147180559945309417
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.785398163397448309616
#define M_1_PI      0.318309886183790671538
#define M_2_PI      0.636619772367581343076
#define M_2_SQRTPI  1.12837916709551257390
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.707106781186547524401

/* Special values */
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))

/* Classification macros */
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isfinite(x)   __builtin_isfinite(x)
#define isinf(x)      __builtin_isinf(x)
#define isnan(x)      __builtin_isnan(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)

static inline int abs(const int x) {
  return x < 0 ? -x : x;
}

static inline uint32_t isqrt(uint32_t n) {
  uint32_t res = 0;
  uint32_t bit = 1U << 30;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

/* Fast absolute value using bit manipulation */
static inline double fabs(double x) {
  union {
    double d;
    uint64_t i;
  } u = {x};
  u.i &= 0x7FFFFFFFFFFFFFFF;
  return u.d;
}

static inline float fabsf(float x) {
  union {
    float f;
    uint32_t i;
  } u = {x};
  u.i &= 0x7FFFFFFF;
  return u.f;
}

/* Fast sign extraction */
static inline double copysign(double x, double y) {
  union {
    double d;
    uint64_t i;
  } ux = {x}, uy = {y};
  ux.i = (ux.i & 0x7FFFFFFFFFFFFFFF) | (uy.i & 0x8000000000000000);
  return ux.d;
}

/* Fast inverse square root (Quake-style with Newton refinement) */
static inline float rsqrtf(float x) {
  union {
    float f;
    uint32_t i;
  } conv = {x};
  conv.i = 0x5f3759df - (conv.i >> 1);
  conv.f *= (1.5f - (x * 0.5f * conv.f * conv.f));
  conv.f *= (1.5f - (x * 0.5f * conv.f * conv.f)); // Second iteration for accuracy
  return conv.f;
}

/* Fast square root using rsqrt */
static inline float fast_sqrtf(float x) {
  return x * rsqrtf(x);
}

/* Optimized square root using x86_64 SSE */
static inline double sqrt(double x) {
  double result;
  __asm__ volatile("sqrtsd %1, %0" : "=x"(result) : "x"(x));
  return result;
}

static inline float sqrtf(float x) {
  float result;
  __asm__ volatile("sqrtss %1, %0" : "=x"(result) : "x"(x));
  return result;
}

/* Fast floor/ceil using SSE4.1 if available */
static inline double floor(double x) {
  double result;
  __asm__ volatile("roundsd $1, %1, %0" : "=x"(result) : "x"(x));
  return result;
}

static inline double ceil(double x) {
  double result;
  __asm__ volatile("roundsd $2, %1, %0" : "=x"(result) : "x"(x));
  return result;
}

static inline double trunc(double x) {
  double result;
  __asm__ volatile("roundsd $3, %1, %0" : "=x"(result) : "x"(x));
  return result;
}

static inline double round(double x) {
  double result;
  __asm__ volatile("roundsd $0, %1, %0" : "=x"(result) : "x"(x));
  return result;
}

/* Fast modulo using bit tricks for powers of 2 */
static inline int64_t fast_mod_pow2(int64_t x, int64_t pow2) {
  return x & (pow2 - 1);
}

/* Efficient fmod implementation */
static inline double fmod(double x, double y) {
  return x - trunc(x / y) * y;
}

/* Fast exp approximation using bit manipulation */
static inline double fast_exp(double x) {
  union {
    double d;
    int64_t i;
  } u;
  u.i = (int64_t) (1512775 * x + 1072632447) << 32;
  return u.d;
}

/* More accurate exp using Taylor series with range reduction */
static inline double exp(double x) {
  if (x < -700) return 0;
  if (x > 700) return INFINITY;

  // Range reduction: exp(x) = exp(x/2^k)^(2^k)
  int k = 0;
  double t = fabs(x);
  while (t > 0.5) {
    t *= 0.5;
    k++;
  }

  if (x < 0) t = -t;

  // Taylor series around 0
  double result = 1.0;
  double term = 1.0;
  for (int i = 1; i < 20; i++) {
    term *= t / i;
    result += term;
    if (fabs(term) < 1e-15) break;
  }

  // Reverse range reduction
  while (k-- > 0) result *= result;

  return (x < 0) ? 1.0 / result : result;
}

/* Fast log using bit manipulation and polynomial approximation */
static inline double log(double x) {
  if (x <= 0) return NAN;
  if (x == 1.0) return 0;

  union {
    double d;
    uint64_t i;
  } u = {x};
  int64_t exp = ((u.i >> 52) & 0x7FF) - 1023;
  u.i = (u.i & 0x000FFFFFFFFFFFFF) | 0x3FF0000000000000;

  double f = u.d - 1.0;
  double s = f / (2.0 + f);
  double s2 = s * s;
  double s4 = s2 * s2;

  // Polynomial approximation
  double t = 2.0 * s * (1.0 + s2 * (0.6666666666666666 + s2 * (0.4 + s2 * 0.285714285714285714)));

  return t + exp * M_LN2;
}

static inline double log2(double x) {
  return log(x) * M_LOG2E;
}

static inline double log10(double x) {
  return log(x) * M_LOG10E;
}

/* Fast power using exp and log */
static inline double pow(double x, double y) {
  if (y == 0) return 1;
  if (y == 1) return x;
  if (y == 2) return x * x;
  if (y == -1) return 1 / x;

  // Special cases
  if (x == 0) return (y > 0) ? 0 : INFINITY;
  if (x < 0 && fmod(y, 1.0) != 0) return NAN;

  double result = exp(y * log(fabs(x)));

  // Handle negative base with integer exponent
  if (x < 0 && fmod(y, 2.0) == 1.0) result = -result;

  return result;
}

/* Fast integer power */
static inline double ipow(double base, int exp) {
  double result = 1.0;
  int abs_exp = exp < 0 ? -exp : exp;

  while (abs_exp) {
    if (abs_exp & 1) result *= base;
    base *= base;
    abs_exp >>= 1;
  }

  return exp < 0 ? 1.0 / result : result;
}

/* Trigonometric functions using Taylor series and range reduction */
static inline double sin(double x) {
  // Range reduction to [-pi, pi]
  x = fmod(x, 2 * M_PI);
  if (x > M_PI) x -= 2 * M_PI;
  else if (x < -M_PI) x += 2 * M_PI;

  // Further reduce to [-pi/2, pi/2]
  int negate = 0;
  if (x > M_PI_2) {
    x = M_PI - x;
    negate = 0;
  } else if (x < -M_PI_2) {
    x = -M_PI - x;
    negate = 0;
  }

  // Taylor series
  double x2 = x * x;
  double result = x;
  double term = x;

  for (int i = 1; i < 10; i++) {
    term *= -x2 / ((2 * i) * (2 * i + 1));
    result += term;
    if (fabs(term) < 1e-15) break;
  }

  return negate ? -result : result;
}

static inline double cos(double x) {
  return sin(x + M_PI_2);
}

static inline double tan(double x) {
  double c = cos(x);
  if (fabs(c) < 1e-15) return copysign(INFINITY, sin(x));
  return sin(x) / c;
}

/* Fast approximate sine using polynomial */
static inline float fast_sinf(float x) {
  // Normalize to [-pi, pi]
  while (x > M_PI) x -= 2 * M_PI;
  while (x < -M_PI) x += 2 * M_PI;

  float x2 = x * x;
  return x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.0001984f)));
}


static inline double atan(double x) {
  int invert = 0, complement = 0;
  double a = fabs(x);

  if (a > 1.0) {
    a = 1.0 / a;
    invert = 1;
  }

  // Polynomial approximation
  double a2 = a * a;
  double result = a * (1.0 - a2 * (0.333333333 - a2 * (0.2 - a2 * 0.142857142857)));

  if (invert) result = M_PI_2 - result;
  return copysign(result, x);
}


static inline double atan2(double y, double x) {
  if (x > 0) return atan(y / x);
  if (x < 0 && y >= 0) return atan(y / x) + M_PI;
  if (x < 0 && y < 0) return atan(y / x) - M_PI;
  if (x == 0 && y > 0) return M_PI_2;
  if (x == 0 && y < 0) return -M_PI_2;
  return 0; // x == 0 && y == 0
}

/* Inverse trig using Newton-Raphson */
static inline double asin(double x) {
  if (x < -1 || x > 1) return NAN;
  if (x == 1) return M_PI_2;
  if (x == -1) return -M_PI_2;

  // Use identity: asin(x) = atan(x/sqrt(1-x^2))
  return atan2(x, sqrt(1 - x * x));
}

static inline double acos(double x) {
  if (x < -1 || x > 1) return NAN;
  return M_PI_2 - asin(x);
}

/* Hyperbolic functions */
static inline double sinh(double x) {
  double e = exp(x);
  return (e - 1.0 / e) * 0.5;
}

static inline double cosh(double x) {
  double e = exp(x);
  return (e + 1.0 / e) * 0.5;
}

static inline double tanh(double x) {
  if (x > 20) return 1.0;
  if (x < -20) return -1.0;
  double e2 = exp(2 * x);
  return (e2 - 1) / (e2 + 1);
}

/* Fast reciprocal approximation */
static inline float fast_recipf(float x) {
  union {
    float f;
    uint32_t i;
  } conv = {x};
  conv.i = 0x7EF127EA - conv.i;
  conv.f *= 2.0f - x * conv.f;
  conv.f *= 2.0f - x * conv.f; // Newton refinement
  return conv.f;
}

/* Cube root using bit manipulation and Newton */
static inline double cbrt(double x) {
  if (x == 0) return 0;

  int neg = x < 0;
  x = fabs(x);

  // Initial guess using bit manipulation
  union {
    double d;
    uint64_t i;
  } u = {x};
  u.i = (u.i / 3) + 0x2A9F7893A596A600LL;

  // Newton refinement
  double r = u.d;
  r = (2.0 * r + x / (r * r)) / 3.0;
  r = (2.0 * r + x / (r * r)) / 3.0;

  return neg ? -r : r;
}

/* Error and gamma functions (simplified) */
static inline double erf(double x) {
  // Approximation using Abramowitz and Stegun
  double a1 = 0.254829592;
  double a2 = -0.284496736;
  double a3 = 1.421413741;
  double a4 = -1.453152027;
  double a5 = 1.061405429;
  double p = 0.3275911;

  int sign = x < 0 ? -1 : 1;
  x = fabs(x);

  double t = 1.0 / (1.0 + p * x);
  double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-x * x);

  return sign * y;
}

static inline double erfc(double x) {
  return 1.0 - erf(x);
}

/* Useful bit manipulation utilities */
static inline int ilogb(double x) {
  union {
    double d;
    uint64_t i;
  } u = {x};
  return ((u.i >> 52) & 0x7FF) - 1023;
}

static inline double ldexp(double x, int exp) {
  union {
    double d;
    uint64_t i;
  } u = {x};
  int current_exp = ((u.i >> 52) & 0x7FF);
  current_exp += exp;
  u.i = (u.i & 0x800FFFFFFFFFFFFF) | ((uint64_t) current_exp << 52);
  return u.d;
}

static inline double frexp(double x, int *exp) {
  union {
    double d;
    uint64_t i;
  } u = {x};
  *exp = ((u.i >> 52) & 0x7FF) - 1022;
  u.i = (u.i & 0x800FFFFFFFFFFFFF) | 0x3FE0000000000000;
  return u.d;
}

/* Fast 2D/3D vector operations using SSE */
typedef struct {
  float x, y;
} vec2f;

typedef struct {
  float x, y, z;
} vec3f;

typedef struct {
  double x, y;
} vec2d;

typedef struct {
  double x, y, z;
} vec3d;

static inline float vec2f_dot(vec2f a, vec2f b) {
  return a.x * b.x + a.y * b.y;
}

static inline float vec3f_dot(vec3f a, vec3f b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3f vec3f_cross(vec3f a, vec3f b) {
  return (vec3f){
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x
  };
}

static inline float vec2f_length(vec2f v) {
  return sqrtf(v.x * v.x + v.y * v.y);
}

static inline float vec3f_length(vec3f v) {
  return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static inline vec2f vec2f_normalize(vec2f v) {
  float invlen = rsqrtf(v.x * v.x + v.y * v.y);
  return (vec2f){v.x * invlen, v.y * invlen};
}

static inline vec3f vec3f_normalize(vec3f v) {
  float invlen = rsqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
  return (vec3f){v.x * invlen, v.y * invlen, v.z * invlen};
}

static inline double lerp(double a, double b, double t) {
  return a + (b - a) * t;
}

static inline double smoothstep(double edge0, double edge1, double x) {
  x = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

/* Fast random number generation (xorshift) */
static inline uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return *state = x;
}

static inline double random_double(uint64_t *state) {
  return (xorshift64(state) & 0x1FFFFFFFFFFFFF) * (1.0 / 9007199254740992.0);
}
