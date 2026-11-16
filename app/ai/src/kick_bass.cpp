#include "kick_bass.hpp"
#include <algorithm>
#include <cmath>

namespace core::ai {

namespace {
double db(double v) { return v > 1e-6 ? 20.0 * std::log10(v) : -120.0; }
// The low end: two one-pole low-passes at 150 Hz.
std::vector<double> lowEnd(const std::vector<float>& x, double sr) {
  const double a = 1.0 - std::exp(-2.0 * 3.14159265358979 * 150.0 / sr);
  std::vector<double> y(x.size());
  double s1 = 0, s2 = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double v = std::isfinite(x[i]) ? x[i] : 0.0;
    s1 += (v - s1) * a;
    s2 += (s1 - s2) * a;
    y[i] = s2;
  }
  return y;
}
// The level in 5 ms blocks (the peak of each: a 55 Hz wave has a peak in every one of 10 ms), so the overlap is
// measured without a release smearing it.
constexpr double kBlockMs = 5.0;
std::vector<double> blocks(const std::vector<float>& x, size_t n, double sr) {
  const size_t w = std::max<size_t>(1, (size_t)(kBlockMs * 0.001 * sr));
  std::vector<double> e((n + w - 1) / w, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const double v = std::isfinite(x[i]) ? std::abs((double)x[i]) : 0.0;
    e[i / w] = std::max(e[i / w], v);
  }
  // A peak in either neighbour counts (a low note's peaks can be 9 ms apart).
  std::vector<double> held(e.size());
  for (size_t i = 0; i < e.size(); ++i) held[i] = std::max({e[i], i ? e[i - 1] : 0.0, i + 1 < e.size() ? e[i + 1] : 0.0});
  return held;
}
}  // namespace

KickBass analyzeKickBass(const std::vector<float>& kick, const std::vector<float>& bass, double sampleRate, double bpm) {
  KickBass r;
  const size_t n = std::min(kick.size(), bass.size());
  if (n == 0 || sampleRate <= 0) {
    r.silentKick = r.silentBass = true;
    return r;
  }
  double pk = 0, pb = 0, ps = 0;
  for (size_t i = 0; i < n; ++i) {
    const double k = std::isfinite(kick[i]) ? kick[i] : 0.0, b = std::isfinite(bass[i]) ? bass[i] : 0.0;
    pk = std::max(pk, std::abs(k));
    pb = std::max(pb, std::abs(b));
    ps = std::max(ps, std::abs(k + b));
  }
  r.kickPeakDb = db(pk);
  r.bassPeakDb = db(pb);
  r.sumPeakDb = db(ps);
  r.silentKick = pk < 1e-4;
  r.silentBass = pb < 1e-4;
  if (r.silentKick || r.silentBass) return r;
  const auto lk = lowEnd(std::vector<float>(kick.begin(), kick.begin() + (long)n), sampleRate);
  const auto lb = lowEnd(std::vector<float>(bass.begin(), bass.begin() + (long)n), sampleRate);
  double kb = 0, kk = 0, bb = 0;
  for (size_t i = 0; i < n; ++i) {
    kb += lk[i] * lb[i];
    kk += lk[i] * lk[i];
    bb += lb[i] * lb[i];
  }
  r.correlation = kk > 0 && bb > 0 ? std::clamp(kb / std::sqrt(kk * bb), -1.0, 1.0) : 0.0;
  const auto ek = blocks(kick, n, sampleRate), eb = blocks(bass, n, sampleRate);
  const double tk = pk * std::pow(10.0, -18.0 / 20.0), tb = pb * std::pow(10.0, -18.0 / 20.0);
  size_t both = 0;
  for (size_t i = 0; i < ek.size(); ++i) both += ek[i] > tk && eb[i] > tb;
  const double beats = bpm > 0 ? (double)n / sampleRate * bpm / 60.0 : 1.0;
  r.overlapMs = (double)both * kBlockMs / std::max(beats, 1.0);
  return r;
}

nlohmann::json KickBass::toJson() const {
  auto round1 = [](double v) { return std::round(v * 10.0) / 10.0; };
  nlohmann::json j = {{"correlacao", std::round(correlation * 100.0) / 100.0}, {"sobreposicao_ms_por_tempo", round1(overlapMs)},
                      {"pico_kick_db", round1(kickPeakDb)}, {"pico_bass_db", round1(bassPeakDb)}, {"pico_soma_db", round1(sumPeakDb)}};
  std::string advice;
  if (silentKick || silentBass) advice = silentKick ? "o kick não toca aqui" : "o bass não toca aqui";
  else {
    if (correlation < -0.1) advice += "fases opostas no grave (cancelam-se): muda o início das notas do bass ou o pitch; ";
    if (overlapMs > 40) advice += "o bass toca por cima do kick: gate psy-bass-gap no bass, ou duck; ";
    if (sumPeakDb > -0.3) advice += "a soma passa -0.3 dBFS: baixa o bass; ";
    if (advice.empty()) advice = "kick e bass estão bem separados";
    else advice.resize(advice.size() - 2);
  }
  j["conselho"] = advice;
  return j;
}

}  // namespace core::ai
