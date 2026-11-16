// Verificacao no Windows com o Vital verdadeiro (nao entra no gate "merge": precisa do Vital.vst3 instalado).
//   vital_check make <Vital.vst3> <presets/vital> <pasta>   -> projetos: init (sem estado), boom, bell, kbbb
//   (correr: core-engine render <pasta>/<nome>.json <pasta>/<nome>.wav para cada um)
//   vital_check judge <pasta>                                -> confere os WAV: o Vital aceitou o estado de cada preset
// O teste: o mesmo Vital, as mesmas notas; so muda o estado. Se o Vital ignorasse o estado, os 4 soavam ao init.
#include <cmath>
#include <cstring>
#include <map>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "edit.hpp"
#include "vital_presets.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
struct Wav { std::vector<double> mono; int sr = 48000; };

Wav readWav(const fs::path& p) {  // PCM 16/24 bits or float 32, any channel count (averaged)
  std::ifstream in(p, std::ios::binary);
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), {});
  if (b.size() < 44 || std::string((char*)b.data(), 4) != "RIFF") throw std::runtime_error(p.string() + ": not a WAV");
  auto u16 = [&](size_t i) { return b[i] | b[i + 1] << 8; };
  auto u32 = [&](size_t i) { return (unsigned)(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (unsigned)b[i + 3] << 24); };
  int fmt = 1, ch = 2, bits = 24;
  Wav w;
  for (size_t i = 12; i + 8 <= b.size();) {
    const std::string id((char*)&b[i], 4);
    const unsigned len = u32(i + 4);
    if (id == "fmt ") { fmt = u16(i + 8); ch = u16(i + 10); w.sr = (int)u32(i + 12); bits = u16(i + 22); if (fmt == 0xFFFE) fmt = u16(i + 32); }
    if (id == "data") {
      const size_t bytes = bits / 8, frames = std::min<size_t>(len, b.size() - i - 8) / (bytes * ch);
      for (size_t f = 0; f < frames; ++f) {
        double s = 0;
        for (int c = 0; c < ch; ++c) {
          const unsigned char* q = &b[i + 8 + (f * ch + c) * bytes];
          if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, q, 4); s += v; }
          else if (bits == 16) s += (int16_t)(q[0] | q[1] << 8) / 32768.0;
          else if (bits == 24) { int v = q[0] | q[1] << 8 | q[2] << 16; if (v & 0x800000) v -= 0x1000000; s += v / 8388608.0; }
        }
        w.mono.push_back(s / ch);
      }
    }
    i += 8 + len + (len & 1);
  }
  return w;
}

struct Facts { double peak = 0, zcr = 0, busy = 0; };
Facts facts(const Wav& w) {  // peak; zero crossings per second (pitch/brightness); share of time above -40 dB
  Facts f;
  for (double s : w.mono) f.peak = std::max(f.peak, std::abs(s));
  int z = 0, loud = 0;
  for (size_t i = 1; i < w.mono.size(); ++i) {
    if ((w.mono[i - 1] < 0) != (w.mono[i] < 0) && std::abs(w.mono[i]) > f.peak * 0.01) ++z;
    if (std::abs(w.mono[i]) > f.peak * 0.01) ++loud;
  }
  const double secs = (double)w.mono.size() / w.sr;
  f.zcr = z / std::max(secs, 1e-9);
  f.busy = (double)loud / std::max<size_t>(w.mono.size(), 1);
  return f;
}

int make(const fs::path& vst3, const fs::path& lib, const fs::path& out) {
  fs::create_directories(out);
  const auto presets = loadVitalLibrary(lib, "");
  const json plugin = {{"format", "vst3"}, {"id", ""}, {"name", "Vital"}, {"vendor", "Vital Audio"}, {"file", vst3.string()}};
  struct Case { const char* name; const char* preset; int pitch; int64_t len; };
  const Case cases[] = {{"init", "", 52, kPpq / 2}, {"boom", "SLA Low Boom", 28, kPpq / 2},
                        {"bell", "HT Glass Bell", 76, kPpq / 8}, {"kbbb", "DPC KBBB Plonk", 28, 150}};
  for (const auto& c : cases) {
    Document d = newEmptyDocument();
    d.bpm = 190;
    d.lengthTicks = 4 * 4 * kPpq;
    d.tracks.clear();
    json pl = plugin;
    const std::string t = addPluginTrack(d, pl);
    Track& tr = trackById(d, t);
    tr.patterns[0].lengthTicks = 4 * kPpq;
    int n = 0;
    for (int64_t tick = kPpq / 4; tick < 4 * kPpq; tick += kPpq / 4)
      if (std::string(c.name) == "kbbb" ? (tick % kPpq != 0) : (tick % kPpq == kPpq / 4))
        tr.patterns[0].notes.push_back({"n" + std::to_string(++n), tick, c.len, c.pitch, 0.9});
    if (*c.preset) setVitalPreset(d, t, *findVitalPreset(presets, c.preset));
    std::ofstream(out / (std::string(c.name) + ".json")) << toProjection(d).dump();
    std::cout << out / (std::string(c.name) + ".json") << '\n';
  }
  return 0;
}

int judge(const fs::path& dir) {
  std::map<std::string, Facts> f;
  for (const char* n : {"init", "boom", "bell", "kbbb"}) {
    f[n] = facts(readWav(dir / (std::string(n) + ".wav")));
    std::printf("%-5s pico %6.1f dB  cruzamentos/s %7.0f  tempo com som %3.0f%%\n", n, 20 * std::log10(f[n].peak + 1e-12),
                f[n].zcr, 100 * f[n].busy);
  }
  int bad = 0;
  auto need = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "OK    " : "FALHOU", what); bad += !ok; };
  for (auto& [n, x] : f) need(x.peak > 0.01, (n + ": tem som").c_str());
  need(f["boom"].zcr < f["init"].zcr * 0.5, "boom: muito mais grave que o init (o Vital aceitou o estado)");
  need(f["bell"].zcr > f["init"].zcr * 1.5, "bell: muito mais agudo que o init (o Vital aceitou o estado)");
  need(f["kbbb"].busy < 0.6, "kbbb: notas curtas, com silencio entre elas");
  std::printf(bad ? "\nFALHOU %d\n" : "\nVERDE\n", bad);
  return bad ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "make" && argc == 5) return make(fs::u8path(argv[2]), fs::u8path(argv[3]), fs::u8path(argv[4]));
    if (mode == "judge" && argc == 3) return judge(fs::u8path(argv[2]));
    std::cerr << "usage: vital_check make <Vital.vst3> <presets/vital> <dir> | judge <dir>\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
