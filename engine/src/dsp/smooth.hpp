#pragma once
#include <algorithm>
#include <cmath>

namespace core {
// A finite glide: tiny remaining distances snap to the exact target, including zero.
// This keeps zero-valued switches/envelopes from spending seconds in silent denormals.
inline double glideTo(double current, double target, double coefficient) {
  const double distance = target - current;
  return std::abs(distance) <= 1e-9 * std::max(1.0, std::abs(target))
             ? target : current + distance * coefficient;
}
} // namespace core
