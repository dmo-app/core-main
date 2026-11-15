// AVX kernel for the saturation 2 up-sampler. Only this unit is compiled with AVX (and no FMA, no
// contraction): the rest of the engine stays on the baseline x86-64 instructions and calls this only after
// cpuRunsAvx(). See oversample_simd.hpp.
#include "oversample_simd.hpp"
#include <immintrin.h>

namespace core {

namespace {

void upsampleAvx(const UpsampleTaps& taps, const float* x, double out[4]) {
  __m256d acc = _mm256_setzero_pd();
  for (int j = 0; j < 16; ++j) {
    const __m256d h = _mm256_load_pd(taps.row[j]);
    const __m256d v = _mm256_set1_pd((double)x[j]);
    acc = _mm256_add_pd(acc, _mm256_mul_pd(h, v));  // lane p: acc_p += h[4j + p] * x[j], as the scalar loop
  }
  alignas(32) double a[4];
  _mm256_store_pd(a, acc);
  a[0] += taps.last * (double)x[16];  // phase 0 has a 17th tap
  out[0] = a[0] * 4;
  out[1] = a[1] * 4;
  out[2] = a[2] * 4;
  out[3] = a[3] * 4;
}

} // namespace

UpsampleKernel upsampleKernelAvx() { return &upsampleAvx; }

} // namespace core
