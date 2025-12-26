#pragma once

#include <kernel/types.h>

typedef struct {
  bool sse;
  bool sse2;
  bool sse3;
  bool ssse3;
  bool sse41;
  bool sse42;
  bool bmi1;
  bool bmi2;
  bool fma;
  bool xsave;
  bool osxsave;
  bool avx;
  bool avx2;
  bool avx512f;
  bool pat;
  bool la57;
  bool nx;
} cpu_features_t;

void cpu_features_init(void);
void cpu_features_init_ap(void);
void cpu_features_dump(cpu_features_t *features);
cpu_features_t *get_cpu_features(void);