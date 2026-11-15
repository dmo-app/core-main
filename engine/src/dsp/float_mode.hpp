// The numeric mode the audio runs in: denormals flushed to zero (FTZ) and read as zero (DAZ) on x86, the way the
// device callback runs (miniaudio sets both there). Audit 2026-11-07 #16: the graph's workers ran without them,
// so a filter that died away kept subnormal states on a worker (Low6: 3.35 ms per million samples instead of 2.47)
// and sounded a hair different from the same track on the callback thread. One rule now: the callback sets it
// (setAudioFloatMode), and a worker always takes the mode of the thread that hands it work (floatMode()), so a
// graph plays the same whether a track ran on the callback or on a worker. The export keeps the mode of its own
// thread, as before (its files stay bit for bit what they were; the golden export checks that).
#pragma once
#include <cstdint>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define CORE_HAS_MXCSR 1
#endif

namespace core {

constexpr uint32_t kFlushDenormals = 0x8040;  // MXCSR FTZ (bit 15) | DAZ (bit 6)

inline uint32_t floatMode() {
#ifdef CORE_HAS_MXCSR
  return _mm_getcsr();
#else
  return 0;
#endif
}
inline void setFloatMode(uint32_t mode) {
#ifdef CORE_HAS_MXCSR
  if (_mm_getcsr() != mode) _mm_setcsr(mode);
#else
  (void)mode;
#endif
}
// FTZ + DAZ on this thread (the callback, the export's thread).
inline void setAudioFloatMode() { setFloatMode(floatMode() | kFlushDenormals); }
inline bool audioFloatMode() {
#ifdef CORE_HAS_MXCSR
  return (floatMode() & kFlushDenormals) == kFlushDenormals;
#else
  return true;
#endif
}

}  // namespace core
