// The 4x up-sampler of saturation 2 in a 256-bit vector (AVX): the four polyphase outputs of one base
// sample are summed side by side, one phase per lane, with the same products in the same order as the
// scalar loop (multiply, then add; no fused multiply-add), so the PCM is bit for bit the same.
// The path is chosen when the effect is built, never in the audio callback; playback and export build
// effects the same way, so they take the same path. CORE_DSP_SIMD=scalar at start-up forces the reference.
#pragma once

namespace core {

// The up-sampler's 65 taps by phase: row j holds h[4j + p] in lane p (j < 16); h[64] is phase 0's last.
struct UpsampleTaps {
  alignas(32) double row[16][4];
  double last;
};

// Writes the 4 outputs at 4x for history x[0..16] (x[0] newest), each already times 4 (the gain of the
// zero-stuffing), exactly like the scalar Upsampler.
using UpsampleKernel = void (*)(const UpsampleTaps& taps, const float* x, double out[4]);

enum class DspPath { Scalar, Avx };

// The AVX kernel, or nullptr when this build has none (CORE_DSP_AVX off, not x86-64).
UpsampleKernel upsampleKernelAvx();

// Whether this CPU and its OS run AVX (CPUID AVX + OSXSAVE, and the OS saves XMM and YMM: XCR0 bits 1-2).
bool cpuRunsAvx();

// The choice, as a pure function (tested): AVX when built, supported and not forced off by env
// ("scalar"; anything else, unset or "auto" means automatic).
DspPath resolveDspPath(bool built, bool supported, const char* env);

// The path new effects take: resolved once from this machine and CORE_DSP_SIMD, unless a test forced it.
DspPath dspPath();
const char* dspPathName(DspPath path);
// Tests only: the path for the effects built from now on (an AVX request on a machine without it stays scalar).
void forceDspPathForTests(DspPath path);

} // namespace core
