#include "sample_analysis.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <vector>
#include "assets.hpp"

namespace core::doc {

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.141592653589793;
constexpr int kFft = 2048;

void fft(std::vector<std::complex<double>>& a) {  // in place, radix 2
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / (double)len;
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// The file's first seconds, one vector per channel.
std::vector<std::vector<float>> readChannels(const fs::path& file, double maxSeconds, double& sampleRate, bool& complete,
                                            const std::atomic<bool>* stop) {
  const WavInfo w = readWavInfo(file);
  std::ifstream in(file, std::ios::binary);
  if (!in || !in.seekg((std::streamoff)w.dataOffset)) throw DocumentError("file", "cannot be read");
  sampleRate = w.sampleRate;
  const int64_t frames = std::min<int64_t>(w.frames, (int64_t)(w.sampleRate * maxSeconds));
  complete = frames == w.frames;
  const int bytes = w.bits / 8, ch = w.channels;
  std::vector<std::vector<float>> out((size_t)ch, std::vector<float>((size_t)frames));
  std::vector<unsigned char> buf((size_t)(4096 * bytes * ch));
  auto sample = [&](const unsigned char* p) -> float {
    if (w.isFloat) {
      if (bytes == 4) { float f; std::memcpy(&f, p, 4); return std::isfinite(f) ? f : 0.0f; }
      double d; std::memcpy(&d, p, 8); return std::isfinite(d) ? (float)d : 0.0f;
    }
    if (bytes == 2) return (float)((int16_t)(p[0] | (p[1] << 8)) / 32768.0);
    if (bytes == 3) return (float)((int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.0);
    return (float)((int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)) / 2147483648.0);
  };
  for (int64_t done = 0; done < frames; done += 4096) {
    if (stop && stop->load()) throw DocumentError("analysis", "stopped");
    const int64_t n = std::min<int64_t>(4096, frames - done);
    if (!in.read((char*)buf.data(), (std::streamsize)(n * bytes * ch))) {  // shorter than its header says
      for (auto& c : out) c.resize((size_t)done);
      complete = false;
      break;
    }
    for (int64_t i = 0; i < n; ++i)
      for (int c = 0; c < ch; ++c) out[(size_t)c][(size_t)(done + i)] = sample(&buf[(size_t)((i * ch + c) * bytes)]);
  }
  return out;
}

double round1(double x) { return std::round(x * 10) / 10; }

// YIN (de Cheveigné & Kawahara 2002): the cumulative mean normalized difference (CMND) of the mono sum over a
// 2048-frame window from `from`, lags up to 25 Hz. The deepest dip wins if it is clear (under 0.2), but an
// earlier dip almost as deep at a whole fraction of its lag wins over it (a sub-octave is the usual mistake),
// refined by a parabola. 0 when no dip is clear: no pitch (noise, a hat, a chord, a fast glide).
// Checked on D:\Samples' basses (2026-11-06): agrees with the names that are right (24 of 25 Psiger notes)
// and with a plain autocorrelation where the names are wrong.
double measurePitch(const std::vector<std::vector<float>>& ch, double sr, size_t from, size_t to) {
  if (sr <= 0 || ch.empty() || to <= from) return 0;
  const size_t maxLag = (size_t)(sr / 25), minLag = std::max<size_t>(2, (size_t)(sr / 1000));
  const size_t avail = to - from;
  if (avail < 2 * minLag + 16) return 0;
  const size_t lagCap = std::min(maxLag, avail / 2);
  const size_t window = std::min<size_t>(2048, avail - lagCap);
  std::vector<double> x(window + lagCap);
  for (size_t i = 0; i < x.size(); ++i) {
    double v = 0;
    for (const auto& c : ch) v += c[from + i];
    x[i] = v;
  }
  std::vector<double> cm(lagCap + 1, 1.0);
  double running = 0;
  for (size_t tau = 1; tau <= lagCap; ++tau) {
    double sum = 0;
    for (size_t i = 0; i < window; ++i) { const double e = x[i] - x[i + tau]; sum += e * e; }
    running += sum;
    cm[tau] = running > 0 ? sum * (double)tau / running : 1.0;
  }
  std::vector<size_t> dips;
  for (size_t tau = minLag; tau < lagCap; ++tau)
    if (cm[tau] <= cm[tau - 1] && cm[tau] <= cm[tau + 1]) dips.push_back(tau);
  if (dips.empty()) return 0;
  size_t best = dips[0];
  for (size_t t : dips) if (cm[t] < cm[best]) best = t;
  if (cm[best] >= 0.2) return 0;
  for (size_t t : dips) {
    if (t >= best) break;
    const double ratio = (double)best / (double)t;
    if (cm[t] <= cm[best] + 0.08 && std::fabs(ratio - std::round(ratio)) < 0.06) { best = t; break; }
  }
  double t = (double)best;
  const double a = cm[best - 1], b = cm[best], c = cm[best + 1], den = a - 2 * b + c;
  if (den > 0) t += 0.5 * (a - c) / den;
  return std::round(sr / t * 10) / 10;
}

}  // namespace

SampleAnalysis analyzeSample(const fs::path& file, const std::string& sha256, const std::atomic<bool>* stop) {
  SampleAnalysis a;
  a.sha256 = sha256;
  double sr = 0;
  const auto ch = readChannels(file, kAnalysisMaxSeconds, sr, a.complete, stop);
  const size_t frames = ch.empty() ? 0 : ch[0].size();
  a.coveredSeconds = sr > 0 ? round1((double)frames / sr * 1000) / 1000 : 0;
  // The envelope: the loudest sample of any channel in each 1 ms.
  const size_t win = std::max<size_t>(1, (size_t)std::lround(sr / 1000));
  std::vector<double> env((frames + win - 1) / win, 0.0);
  for (const auto& c : ch)
    for (size_t i = 0; i < frames; ++i) env[i / win] = std::max(env[i / win], (double)std::fabs(c[i]));
  const double peak = env.empty() ? 0 : *std::max_element(env.begin(), env.end());
  const double threshold = std::max(0.001, peak * std::pow(10.0, -50.0 / 20));  // -60 dBFS, or 50 dB under the peak
  if (peak < 0.001) { a.silent = true; return a; }
  a.peakDb = std::round(20 * std::log10(peak) * 10) / 10;
  size_t first = 0, last = env.size() - 1;
  while (first < env.size() && env[first] < threshold) ++first;
  while (last > first && env[last] < threshold) --last;
  a.leadingSilenceMs = (double)first;
  a.activeMs = (double)(last - first + 1);
  // Attack: to 90% of the loudest point of the first 250 ms.
  const size_t early = std::min(env.size(), first + 250);
  const double top = *std::max_element(env.begin() + (long)first, env.begin() + (long)early);
  size_t reach = first;
  while (reach < early && env[reach] < 0.9 * top) ++reach;
  a.attackMs = (double)(reach - first);
  // For hits and the tail, the envelope held over 20 ms: a low note's zero crossings are not silences.
  std::vector<double> held(env.size(), 0.0);
  for (size_t i = 0; i < env.size(); ++i)
    for (size_t j = i >= 10 ? i - 10 : 0; j <= std::min(env.size() - 1, i + 10); ++j) held[i] = std::max(held[i], env[j]);
  // Hits, with hysteresis: under 25% of the peak arms, over 50% counts.
  bool armed = true;
  for (size_t i = first; i <= last; ++i) {
    if (armed && held[i] >= 0.5 * peak) { ++a.hits; armed = false; }
    else if (held[i] < 0.25 * peak) armed = true;
  }
  if (a.hits <= 1) {
    const size_t loudest = (size_t)(std::max_element(env.begin(), env.end()) - env.begin());
    size_t end = loudest;
    for (size_t i = loudest; i < env.size(); ++i) if (env[i] >= peak * 0.01) end = i;  // the last moment above -40 dB
    a.tailMs = (double)(end + 1 - loudest);
  }
  // Spectrum of the sounding part (at most 10 s of it): mean power of Hann windows, channels added.
  const size_t from = first * win, to = std::min(frames, std::min((last + 1) * win, from + (size_t)(sr * 10)));
  std::vector<double> power(kFft / 2 + 1, 0.0), hann(kFft);
  for (int i = 0; i < kFft; ++i) hann[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * i / (kFft - 1));
  std::vector<std::complex<double>> buf(kFft);
  for (size_t start = from; start < to; start += kFft) {
    if (stop && stop->load()) throw DocumentError("analysis", "stopped");
    for (const auto& c : ch) {
      for (size_t i = 0; i < (size_t)kFft; ++i) buf[i] = start + i < to ? c[start + i] * hann[i] : 0.0;
      fft(buf);
      for (size_t k = 0; k < power.size(); ++k) power[k] += std::norm(buf[k]);
    }
  }
  double total = 0, weighted = 0, sub = 0, high = 0;
  for (size_t k = 1; k < power.size(); ++k) {  // no DC
    const double f = (double)k * sr / kFft;
    total += power[k];
    weighted += f * power[k];
    if (f < 120) sub += power[k];
    if (f > 5000) high += power[k];
  }
  if (total > 0) {
    a.centroidHz = std::round(weighted / total);
    a.subRatio = std::round(sub / total * 1000) / 1000;
    a.highRatio = std::round(high / total * 1000) / 1000;
  }
  // The pitch, 30 ms after the sound starts (past a kick's click or a pluck's attack), to its end.
  a.pitchHz = measurePitch(ch, sr, std::min(frames, from + (size_t)(sr * 0.03)), std::min(frames, (last + 1) * win));
  return a;
}

json analysisToJson(const SampleAnalysis& a) {
  json j = {{"version", a.version}, {"sha256", a.sha256}, {"coveredSeconds", a.coveredSeconds}, {"complete", a.complete}};
  if (a.silent) { j["silent"] = true; return j; }
  j["leadingSilenceMs"] = a.leadingSilenceMs;
  j["activeMs"] = a.activeMs;
  j["attackMs"] = a.attackMs;
  j["tailMs"] = a.tailMs < 0 ? json(nullptr) : json(a.tailMs);
  j["hits"] = a.hits;
  j["centroidHz"] = a.centroidHz;
  j["subRatio"] = a.subRatio;
  j["highRatio"] = a.highRatio;
  j["pitchHz"] = a.pitchHz;
  j["peakDb"] = a.peakDb;
  return j;
}

bool analysisFromJson(const json& j, SampleAnalysis& a) {
  if (!j.is_object() || j.value("version", "") != kAnalyzerVersion) return false;
  try {
    a = SampleAnalysis();
    a.sha256 = j.value("sha256", "");
    a.coveredSeconds = j.value("coveredSeconds", 0.0);
    a.complete = j.value("complete", false);
    a.silent = j.value("silent", false);
    if (a.silent) return true;
    a.leadingSilenceMs = j.at("leadingSilenceMs").get<double>();
    a.activeMs = j.at("activeMs").get<double>();
    a.attackMs = j.at("attackMs").get<double>();
    a.tailMs = j.contains("tailMs") && j["tailMs"].is_number() ? j["tailMs"].get<double>() : -1;
    a.hits = j.value("hits", 0);
    a.centroidHz = j.value("centroidHz", 0.0);
    a.subRatio = j.value("subRatio", 0.0);
    a.highRatio = j.value("highRatio", 0.0);
    a.pitchHz = j.value("pitchHz", 0.0);
    a.peakDb = j.value("peakDb", -120.0);
  } catch (const json::exception&) {
    return false;
  }
  return true;
}

int pitchToMidi(double hz) {
  if (!(hz > 0)) return -1;
  const int m = (int)std::lround(69 + 12 * std::log2(hz / 440.0));
  return m >= 0 && m <= 127 ? m : -1;
}

std::vector<std::string> analysisWords(const SampleAnalysis& a) {
  std::vector<std::string> w;
  if (a.silent) return {"silent"};
  if (a.activeMs < 300) w.push_back("short");
  if (a.activeMs > 2000) w.push_back("long");
  if (a.centroidHz > 3000) w.push_back("bright");
  if (a.centroidHz > 0 && a.centroidHz < 800) w.push_back("dark");
  if (a.subRatio > 0.4) w.push_back("sub");
  return w;
}

}  // namespace core::doc
