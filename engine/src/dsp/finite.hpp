// NaN and Inf read from the bits. The engine is built with /fp:fast on Windows (engine/CMakeLists.txt), where MSVC
// may assume no float is NaN or Inf and fold std::isfinite to true: every check that guards the output (a unit
// that broke, a plugin that sent NaN, the export's report) uses these instead (audit 2026-11-07 #10).
#pragma once
#include <cstdint>
#include <cstring>

namespace core {

inline bool finiteBits(float x) {
  uint32_t bits;
  std::memcpy(&bits, &x, sizeof bits);
  return (bits & 0x7f800000u) != 0x7f800000u;
}
inline bool finiteBits(double x) {
  uint64_t bits;
  std::memcpy(&bits, &x, sizeof bits);
  return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

}  // namespace core
