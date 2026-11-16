#include "sampling.hpp"
#include "dsp/clip_grains.hpp"

#include <algorithm>
#include <cmath>
#include "samples.hpp"

namespace core::sampling {

void shapeClip(std::vector<std::vector<float>>& channels, double sampleRate, double gainDb, double fadeInSeconds,
               double fadeOutSeconds) {
  if (channels.empty() || !(sampleRate > 0)) return;
  const int64_t length = (int64_t)channels[0].size();
  const int64_t fadeIn = std::min<int64_t>(length, std::max<int64_t>(0, std::llround(fadeInSeconds * sampleRate)));
  const int64_t fadeOut = std::min<int64_t>(length, std::max<int64_t>(0, std::llround(fadeOutSeconds * sampleRate)));
  const float gain = (float)std::pow(10.0, gainDb / 20.0);
  for (int64_t i = 0; i < length; ++i) {
    float g = gain;
    if (i < fadeIn) g *= (float)i / (float)fadeIn;
    if (length - i <= fadeOut) g *= (float)(length - i) / (float)fadeOut;  // as in the engine's graph.cpp
    for (auto& ch : channels)
      if (i < (int64_t)ch.size()) ch[(size_t)i] *= g;
  }
}

std::vector<std::vector<float>> processClip(const std::vector<std::vector<float>>& file, double sampleRate, int64_t frames,
                                            double pitchSemitones, double freezeSeconds) {
  std::vector<std::vector<float>> out(file.size(), std::vector<float>((size_t)std::max<int64_t>(0, frames), 0.0f));
  if (file.empty() || file[0].empty() || frames <= 0) return out;
  const int64_t have = (int64_t)file[0].size();
  const ClipGrains g = clipGrains(pitchSemitones, freezeSeconds, sampleRate, 0, have);
  if (g.grain <= 0) {
    for (size_t c = 0; c < file.size(); ++c) std::copy_n(file[c].begin(), std::min<int64_t>(frames, (int64_t)file[c].size()), out[c].begin());
    return out;
  }
  const float* L = file[0].data();
  const float* R = file.size() > 1 ? file[1].data() : file[0].data();  // a mono file: the same on both sides
  for (int64_t i = 0; i < frames; ++i) {
    double l, r;
    clipGrainFrame(g, 0, L, R, have, i, l, r);
    out[0][(size_t)i] = (float)l;
    if (out.size() > 1) out[1][(size_t)i] = (float)r;
  }
  return out;
}

double clipSourceSeconds(double durationSeconds, double pitchSemitones, double freezeSeconds) {
  const double ratio = pitchSemitones != 0 ? std::pow(2.0, std::clamp(pitchSemitones, -24.0, 24.0) / 12.0) : 1.0;
  if (freezeSeconds > 0) return freezeSeconds + 0.1;
  return durationSeconds * std::max(1.0, ratio) + 0.1;  // + a grain, so the last one reads real sound
}

std::string safeFileName(const std::string& name) {
  std::string s;
  for (char ch : name) {
    const unsigned char u = (unsigned char)ch;
    if (u < 32 || std::string("\\/:*?\"<>|").find(ch) != std::string::npos) s += '_';
    else s += ch;
  }
  if (s.size() > 80) {
    s.resize(80);
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();  // no half UTF-8 character
    if (!s.empty() && ((unsigned char)s.back() & 0x80)) s.pop_back();
  }
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();  // after the cut: Windows drops them
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  if (s.empty()) return "sample";
  // Device names (CON, NUL, COM1...) are not files on Windows, even with an extension.
  std::string upper;
  for (char ch : s.substr(0, s.find('.'))) upper += (char)((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
  static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                         "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const char* r : reserved)
    if (upper == r) return "_" + s;
  return s;
}


AudibleRange audibleRange(const Channels& channels, double sampleRate, double thresholdDb, double startMarginMs, double endMarginMs) {
  AudibleRange r;
  if (channels.empty() || !(sampleRate > 0)) return r;
  const int64_t length = (int64_t)channels[0].size();
  const float threshold = (float)std::pow(10.0, thresholdDb / 20.0);
  auto loud = [&](int64_t i) {
    for (const auto& ch : channels)
      if (i < (int64_t)ch.size() && std::abs(ch[(size_t)i]) > threshold) return true;
    return false;
  };
  int64_t first = 0;
  while (first < length && !loud(first)) ++first;
  if (first == length) return r;  // silence
  int64_t last = length - 1;
  while (last > first && !loud(last)) --last;
  r.from = std::max<int64_t>(0, first - std::llround(startMarginMs / 1000.0 * sampleRate));
  r.to = std::min<int64_t>(length, last + 1 + std::llround(endMarginMs / 1000.0 * sampleRate));
  return r;
}

bool trimSilence(Channels& channels, double sampleRate, double thresholdDb, double fadeMs) {
  const AudibleRange r = audibleRange(channels, sampleRate, thresholdDb);
  if (r.to <= r.from) return false;
  const int64_t length = channels.empty() ? 0 : (int64_t)channels[0].size();
  for (auto& ch : channels) {
    const int64_t to = std::min<int64_t>(r.to, (int64_t)ch.size());
    ch = std::vector<float>(ch.begin() + std::min<int64_t>(r.from, to), ch.begin() + to);
  }
  // A cut inside the sound gets a short fade (a cut in silence needs none, but it does no harm there).
  const int64_t n = (int64_t)channels[0].size();
  const int64_t fade = std::min<int64_t>(n / 2, std::llround(fadeMs / 1000.0 * sampleRate));
  if (fade > 0) {
    for (auto& ch : channels)
      for (int64_t i = 0; i < fade && i < (int64_t)ch.size(); ++i) {
        const float g = (float)i / (float)fade;
        if (r.from > 0) ch[(size_t)i] *= g;
        if (r.to < length) ch[ch.size() - 1 - (size_t)i] *= g;
      }
  }
  return true;
}

bool hasBrokenSamples(const Channels& channels) {
  for (const auto& ch : channels)
    for (float v : ch)
      if (!std::isfinite(v)) return true;
  return false;
}

float peakOf(const Channels& channels) {
  float peak = 0;
  for (const auto& ch : channels)
    for (float v : ch) peak = std::max(peak, std::abs(v));
  return peak;
}

double normalizePeak(Channels& channels, double targetDb) {
  const float peak = peakOf(channels);
  if (!(peak > 1e-6f) || !std::isfinite(peak)) return 0;
  const double db = targetDb - 20.0 * std::log10((double)peak);
  const float gain = (float)std::pow(10.0, db / 20.0);
  for (auto& ch : channels)
    for (float& v : ch) v *= gain;
  return db;
}

std::string roleFromName(const std::string& name) {
  return core::doc::classifySample(safeFileName(name) + ".wav").role;
}

}  // namespace core::sampling
