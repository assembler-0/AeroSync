__uint128_t __udivti3(__uint128_t a, __uint128_t b) {
  __uint128_t q = 0;
  for (int i = 127; i >= 0; i--) {
    if ((b << i) <= a) {
      a -= b << i;
      q |= ((__uint128_t)1 << i);
    }
  }
  return q;
}
