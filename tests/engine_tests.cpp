// Testes do motor (core_engine_lib): sinais gerados, sem fixtures nem goldens presos a um compilador.
// Correr: engine_tests [parte-do-nome]
#include <filesystem>
#include <string>
#include "support/check.hpp"
#include "support/signals.hpp"
#include "app/measure.hpp"
#include "core/assets.hpp"
#include "core/engine.hpp"
#include "core/graph.hpp"
#include "core/project.hpp"
#include "core/qualify.hpp"
#include "core/server.hpp"
#include "dsp/clip_grains.hpp"
#include "io/audio_file.hpp"
#include "io/wav.hpp"

using namespace core;
using namespace coretest;
namespace fs = std::filesystem;

namespace {

const double kSr = 48000;

fs::path tempDir() {
  fs::path d = fs::temp_directory_path() / "core-engine-tests";
  fs::create_directories(d);
  return d;
}

// A song with one audio track playing one file from startTick.
json audioSong(const std::string& path, double durationSeconds, int64_t startTick = 0, double gainDb = 0) {
  json clip = {{"startTick", startTick}, {"path", path}, {"offsetSeconds", 0}, {"durationSeconds", durationSeconds}, {"gainDb", gainDb}};
  json track = {{"id", "audio"}, {"name", "Audio"}, {"instrument", "audio"}, {"audioClips", json::array({clip})}};
  return {{"format", kProjectFormat}, {"version", 2}, {"name", "teste"}, {"bpm", 150.0},
          {"lengthTicks", 4 * 4 * kPpq}, {"masterGainDb", 0}, {"tracks", json::array({track})}};
}

std::string writeSine(const std::string& name, double hz, double seconds, float amp) {
  const auto s = sine(hz, kSr, (int64_t)(seconds * kSr), amp);
  const std::string path = (tempDir() / name).string();
  writeWav(path, s, s, (int)kSr, 32);
  return path;
}

// Um ficheiro com o trecho congelado [offset, offset + span) a `inside` e o resto a 1 (esquerda) e 2 (direita): uma
// saida que mostre o exterior nao pode vir so do trecho.
std::pair<std::vector<float>, std::vector<float>> markedFile(int64_t frames, int64_t offset, int64_t span, float inside) {
  std::vector<float> l((size_t)frames, 1.0f), r((size_t)frames, 2.0f);
  for (int64_t i = offset; i < std::min(frames, offset + span); ++i) { l[(size_t)i] = inside; r[(size_t)i] = inside; }
  return {l, r};
}

}  // namespace

// --- Ficheiros WAV ---------------------------------------------------------------------------------------------

TEST("wav: escrever e ler um seno de 32 bits devolve as mesmas amostras") {
  const auto L = sine(440, kSr, 4800, 0.5f);
  const auto R = noise(4800, 7, 0.25f);
  const std::string path = (tempDir() / "roundtrip.wav").string();
  writeWav(path, L, R, (int)kSr, 32);
  AudioData d = readWav(path, 1 << 20);
  CHECK_EQ(d.frames(), (int64_t)4800);
  CHECK_EQ(d.channels, 2);
  CHECK_NEAR(d.sampleRate, kSr, 0);
  double worst = 0;
  for (size_t i = 0; i < L.size() && i < d.L.size(); ++i)
    worst = std::max({worst, (double)std::fabs(d.L[i] - L[i]), (double)std::fabs(d.R[i] - R[i])});
  CHECK_NEAR(worst, 0, 1e-6);
}

TEST("wav: 24 bits fica a menos de 2 passos de quantizacao (-132 dBFS)") {
  const auto L = sine(1000, kSr, 4800, 0.9f);
  const std::string path = (tempDir() / "q24.wav").string();
  writeWav(path, L, L, (int)kSr, 24);
  AudioData d = readWav(path, 1 << 20);
  double worst = 0;
  for (size_t i = 0; i < L.size() && i < d.L.size(); ++i) worst = std::max(worst, (double)std::fabs(d.L[i] - L[i]));
  CHECK_NEAR(worst, 0, 2.0 / 8388608.0);  // medido 2026-11-16: 1,5 passos (escala 2^23-1 na escrita)
}

TEST("wav: ficheiro inexistente da erro, nao audio vazio") {
  CHECK_THROWS(readWav((tempDir() / "nao-existe.wav").string(), 1 << 20));
}

// --- Tempo -----------------------------------------------------------------------------------------------------

TEST("tempo: samplesPerTick a 150 BPM / 48 kHz") {
  // 150 BPM = 0,4 s por batida = 19200 amostras por 960 ticks = 20 amostras por tick.
  CHECK_NEAR(samplesPerTick(150, 48000), 20.0, 1e-12);
  CHECK_NEAR(samplesPerTick(500, 48000) * kPpq, 48000 * 60.0 / 500, 1e-9);
}

// --- Projecto (JSON) -------------------------------------------------------------------------------------------

TEST("projecto: toJson(fromJson(x)) e estavel") {
  Project p = fromJson(audioSong("a.wav", 1.0));
  const std::string once = toJson(p).dump();
  const std::string twice = toJson(fromJson(json::parse(once))).dump();
  CHECK_EQ(once, twice);
}

TEST("projecto: entrada invalida e recusada") {
  json bad = audioSong("a.wav", 1.0);
  bad["tracks"][0]["id"] = "id com espacos";
  CHECK_THROWS(fromJson(bad));
  json noInstrument = audioSong("a.wav", 1.0);
  noInstrument["tracks"][0].erase("instrument");
  CHECK_THROWS(fromJson(noInstrument));
}

// Regressao (2026-11-16): com a saida dos instrumentos proprios, uma musica antiga com automacao num parametro do
// W-Core ("tone") deixava de abrir ("envelope on unknown parameter 'tone'"). Abre, a automacao sai com uma nota.
TEST("projecto: musica antiga com automacao num instrumento proprio abre") {
  Project p;
  CHECK_EQ((p = fromJson(demoProject(145)), (int)p.tracks.size()), 3);
  bool noted = false;
  for (const auto& t : p.tracks) {
    CHECK_EQ(t.instrument, std::string("none"));
    for (const auto& n : t.loadNotes) noted = noted || n.find("tone") != std::string::npos;
  }
  CHECK(noted);
  // A automacao de mistura (gainDb) de uma pista antiga continua.
  json song = demoProject(145);
  song["tracks"][1]["envelopes"].push_back({{"param", "gainDb"}, {"points", {{{"tick", 0}, {"value", -6}}}}});
  Project q = fromJson(song);
  CHECK_EQ((int)q.tracks[1].envelopes.size(), 1);
  CHECK_EQ(q.tracks[1].envelopes[0].param, std::string("gainDb"));
}

// --- Render ----------------------------------------------------------------------------------------------------

TEST("render: musica sem som da silencio finito com a duracao certa") {
  Project p = fromJson(demoProject(145));
  RenderResult r = renderProject(p, kSr, 512, false, 1.0, nullptr, nullptr, nullptr, 0);
  CHECK(allFinite(r.L) && allFinite(r.R));
  CHECK_NEAR(r.peak, 0, 0);
  const double expected = 8 * 4 * kPpq * samplesPerTick(145, kSr) + kSr;
  CHECK_NEAR((double)r.L.size(), expected, 1);
}

TEST("render: clip de audio toca no sitio, ao nivel certo e sem NaN") {
  const std::string path = writeSine("sine440.wav", 440, 2.0, 0.5f);
  const int64_t startTick = 2 * kPpq;  // 2 batidas a 150 BPM = 0,8 s
  const int64_t start = (int64_t)(startTick * samplesPerTick(150, kSr));  // 38400
  Project p = fromJson(audioSong(path, 1.0, startTick));
  AssetStore assets;
  RenderResult r = renderProject(p, kSr, 512, false, 0.5, nullptr, &assets, nullptr, 0);
  CHECK(allFinite(r.L) && allFinite(r.R));
  CHECK_EQ(start, (int64_t)38400);
  // O seno comeca em 0: a primeira amostra acima do limiar fica a menos de 1 ms do inicio do clip.
  const int64_t onset = firstAbove(r.L, 1e-4f);
  CHECK(onset >= start && onset < start + 48);
  if (onset < start || onset >= start + 48) coretest::fail(__FILE__, __LINE__, coretest::pair(onset, start));
  CHECK_NEAR(r.peak, 0.5, 0.02);  // ganho 0 dB, mistura 0 dB: o nivel do ficheiro
  // Antes do clip, silencio absoluto; depois de 1 s de clip, silencio outra vez.
  CHECK_NEAR(peakOf(std::vector<float>(r.L.begin(), r.L.begin() + start)), 0, 0);
  CHECK_NEAR(peakOf(std::vector<float>(r.L.begin() + start + 48000 + 64, r.L.end())), 0, 1e-6);
}

TEST("render: ganho do clip de -6 dB baixa o pico para metade") {
  const std::string path = writeSine("sine1k.wav", 1000, 1.0, 0.5f);
  AssetStore assets;
  RenderResult a = renderProject(fromJson(audioSong(path, 0.5, 0, 0)), kSr, 512, false, 0.1, nullptr, &assets, nullptr, 0);
  RenderResult b = renderProject(fromJson(audioSong(path, 0.5, 0, -6.0206)), kSr, 512, false, 0.1, nullptr, &assets, nullptr, 0);
  CHECK(a.peak > 0.1f);
  CHECK_NEAR(b.peak / a.peak, 0.5, 0.01);
}

TEST("render: o tamanho do bloco nao muda o resultado") {
  const std::string path = writeSine("sine-block.wav", 220, 1.5, 0.4f);
  Project p = fromJson(audioSong(path, 1.2, kPpq / 3));
  AssetStore assets;
  RenderResult ref = renderProject(p, kSr, 512, false, 0.2, nullptr, &assets, nullptr, 0);
  for (int block : {1, 64, 333, 1024}) {
    RenderResult r = renderProject(p, kSr, block, false, 0.2, nullptr, &assets, nullptr, 0);
    CHECK_EQ(r.L.size(), ref.L.size());
    double worst = 0;
    for (size_t i = 0; i < r.L.size() && i < ref.L.size(); ++i) worst = std::max(worst, (double)std::fabs(r.L[i] - ref.L[i]));
    CHECK_NEAR(worst, 0, 0);
  }
}

TEST("render: deterministico na mesma build (duas vezes igual)") {
  const std::string path = (tempDir() / "noise.wav").string();
  const auto n = noise(48000, 99, 0.3f);
  writeWav(path, n, n, (int)kSr, 32);
  Project p = fromJson(audioSong(path, 1.0, kPpq));
  AssetStore assets;
  RenderResult a = renderProject(p, kSr, 512, false, 0.3, nullptr, &assets, nullptr, -1);
  RenderResult b = renderProject(p, kSr, 512, false, 0.3, nullptr, &assets, nullptr, 0);
  CHECK(a.L == b.L);
  CHECK(a.R == b.R);
}

TEST("render: ficheiro em falta nao parte o render") {
  Project p = fromJson(audioSong((tempDir() / "falta.wav").string(), 1.0));
  AssetStore assets;
  RenderResult r = renderProject(p, kSr, 512, false, 0.1, nullptr, &assets, nullptr, 0);
  CHECK(allFinite(r.L));
  CHECK_NEAR(r.peak, 0, 0);
}

// --- Loop e mudanca de tempo (sweep: o loop era convertido duas vezes) ------------------------------------------

TEST("loop: posto no tempo novo antes de o audio aplicar o grafo novo nao e convertido outra vez") {
  Engine e(kSr);
  std::vector<float> L(256), R(256);
  e.publish(std::make_unique<Graph>(fromJson(demoProject(120)), kSr));
  e.process(L.data(), R.data(), 256);  // o audio tem o grafo a 120
  e.setLoop(true, 48000, 96000);
  e.publish(std::make_unique<Graph>(fromJson(demoProject(240)), kSr));
  e.setLoop(true, 24000, 48000);  // a app manda o loop ja no tempo novo (metade das amostras)
  e.process(L.data(), R.data(), 256);
  CHECK_EQ(e.loopStart(), (int64_t)24000);
  CHECK_EQ(e.loopEnd(), (int64_t)48000);
  // Sem loop novo, a mudanca de tempo continua a levar o loop consigo.
  e.publish(std::make_unique<Graph>(fromJson(demoProject(120)), kSr));
  e.process(L.data(), R.data(), 256);
  CHECK_EQ(e.loopStart(), (int64_t)48000);
  CHECK_EQ(e.loopEnd(), (int64_t)96000);
  // O fim conta a partir do inicio ja limitado a 0.
  e.setLoop(true, -500, -100);
  CHECK_EQ(e.loopStart(), (int64_t)0);
  CHECK(e.loopEnd() >= 1);
}

// --- Freeze (G17 / T25) -----------------------------------------------------------------------------------------
// Um freeze guarda um trecho do ficheiro; a saida so pode ler esse trecho, mesmo quando o trecho e mais curto que o grao
// (10 ms a 48 kHz: 480 amostras, grao de 40 ms = 1920). Antes da correcao a primeira amostra ja vinha de fora.

TEST("freeze: um trecho curto nunca le fora de si (taxas, pitch, inicio e fim do ficheiro)") {
  for (double sr : {44100.0, 48000.0, 96000.0})
    for (double freeze : {0.010, 0.025, 0.050, 0.500})
      for (double semis : {-24.0, -7.0, 0.0, 7.0, 24.0}) {
        const int64_t frames = (int64_t)(2 * sr);
        const int64_t span = (int64_t)std::llround(freeze * sr);
        for (int64_t offset : {(int64_t)0, frames / 3, frames - span}) {
          const auto [l, r] = markedFile(frames, offset, span, 0.0f);
          const ClipGrains g = clipGrains(semis, freeze, sr, offset, frames);
          int64_t firstOutside = -1;
          for (int64_t k = 0; k < (int64_t)sr && firstOutside < 0; ++k) {
            double ol, orr;
            clipGrainFrame(g, offset, l.data(), r.data(), frames, k, ol, orr);
            if (ol != 0 || orr != 0) firstOutside = k;
          }
          if (firstOutside >= 0) {
            CHECK_EQ(firstOutside, (int64_t)-1);
            std::fprintf(stderr, "    sr %.0f, freeze %.3f s, pitch %.0f, offset %lld: a amostra %lld le fora do trecho\n", sr, freeze,
                         semis, (long long)offset, (long long)firstOutside);
            return;
          }
        }
      }
}

TEST("freeze: o trecho curto continua a soar, ao nivel do trecho") {
  const double sr = 48000;
  const int64_t frames = (int64_t)sr, offset = 4800, span = 480;
  const auto [l, r] = markedFile(frames, offset, span, 0.5f);
  const ClipGrains g = clipGrains(0, 0.010, sr, offset, frames);
  double peak = 0, sum = 0;
  for (int64_t k = 0; k < (int64_t)sr; ++k) {
    double ol, orr;
    clipGrainFrame(g, offset, l.data(), r.data(), frames, k, ol, orr);
    CHECK(std::isfinite(ol) && std::isfinite(orr));
    peak = std::max(peak, std::max(std::abs(ol), std::abs(orr)));
    sum += ol;
  }
  CHECK(peak <= 0.5 + 1e-9);  // duas janelas de Hann a meio grao somam 1
  CHECK_NEAR(sum / sr, 0.5, 0.05);  // e o nivel medio fica no do trecho
}

// Qualify: um instrumento cujo acorde normal passa +6 dBFS e recusado ("too loud"); os 66 tons de uma vez do teste
// de extremos nao contam (sfizz sem SFZ deu +20 dBFS, 16/11).
namespace {
class LoudSynth final : public core::PluginProcessor {
public:
  explicit LoudSynth(float perNote) : perNote_(perNote) {}
  void process(float* l, float* r, int frames, const core::MidiNote* notes, int count) override {
    for (int k = 0; k < count; ++k)
      if (notes[k].pitch >= 0) held_ = std::max(0, held_ + (notes[k].on ? 1 : -1));
    for (int i = 0; i < frames; ++i) { l[i] += perNote_ * held_; r[i] += perNote_ * held_; }
  }
  bool isInstrument() const override { return true; }
  int latencySamples() const override { return 0; }
private:
  float perNote_;
  int held_ = 0;
};
class OneProvider final : public core::PluginProvider {
public:
  explicit OneProvider(float perNote) : perNote_(perNote) {}
  std::shared_ptr<core::PluginProcessor> get(const std::string&, const core::json&, double, bool, std::string*) override {
    return std::make_shared<LoudSynth>(perNote_);
  }
  void keepOnly(const std::vector<std::string>&) override {}
private:
  float perNote_;
};
}  // namespace

TEST("qualify: instrumento demasiado alto no acorde normal e recusado; os extremos nao contam") {
  const core::json plugin = {{"name", "Loud"}, {"id", "x"}};
  OneProvider quiet(0.1f);   // acorde de 4: 0.4 (-8 dBFS); 66 notas: 6.6 (+16 dBFS) so nos extremos
  const core::json ok = core::qualifyPlugin(quiet, plugin, 48000, 1);
  CHECK_EQ(ok.value("verdict", std::string()), std::string("ok"));
  CHECK(ok.value("signalPeakDb", 0.0) < 0);
  CHECK(ok.value("peakDb", 0.0) > 12);
  OneProvider loud(0.8f);    // acorde de 4: 3.2 (+10 dBFS)
  const core::json bad = core::qualifyPlugin(loud, plugin, 48000, 1);
  CHECK_EQ(bad.value("verdict", std::string()), std::string("unsafe"));
  CHECK(bad["problems"].dump().find("too loud") != std::string::npos);
}

CORE_TEST_MAIN
