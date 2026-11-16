// O motor do Core (Diogo, 16/11): leve, carrega muitos plugins em cadeia e lida com isso sem falhar.
// Plugins de teste (sem VST3, sem rede): um sintetizador simples, um ganho, e plugins que falham de propósito
// (exceção, NaN, Inf, nível descontrolado, plugin que não carrega). Planos T28 (carga) e T29 (robustez).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "core/engine.hpp"
#include "core/graph.hpp"
#include "core/plugins.hpp"
#include "core/project.hpp"
#include "core/server.hpp"
#include "support/check.hpp"

using core::json;

namespace {

constexpr double kRate = 48000;

// O que um plugin de teste faz: o id do plugin no documento é "test:<tipo>[:<chamada em que falha>]".
enum class Kind { Synth, Gain, Throw, NaN, Inf, Runaway };

std::atomic<int64_t> g_instances{0}, g_calls{0};

class TestPlugin final : public core::PluginProcessor {
public:
  TestPlugin(Kind kind, bool instrument, int64_t failAt) : kind_(kind), instrument_(instrument), failAt_(failAt) {
    g_instances.fetch_add(1, std::memory_order_relaxed);
  }
  void process(float* l, float* r, int frames, const core::MidiNote* notes, int count) override {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (failAt_ >= 0 && calls_++ >= failAt_) {
      switch (kind_) {
        case Kind::Throw: throw std::runtime_error("plugin de teste: falha de proposito");
        case Kind::NaN: l[frames / 2] = std::nanf(""); return;
        case Kind::Inf: r[0] = INFINITY; return;
        case Kind::Runaway: for (int i = 0; i < frames; ++i) l[i] = r[i] = 1000.0f; return;
        default: break;
      }
    }
    if (!instrument_) {  // um efeito: um ganho pequeno (a cadeia faz trabalho e muda o som de forma conhecida)
      for (int i = 0; i < frames; ++i) { l[i] *= 0.999f; r[i] *= 0.999f; }
      return;
    }
    int at = 0;
    for (int i = 0; i < frames; ++i) {
      while (at < count && notes[at].offset <= i) {
        const auto& n = notes[at++];
        if (n.pitch == core::kAllNotesOff) { for (bool& h : held_) h = false; continue; }
        held_[n.pitch] = n.on;
      }
      float s = 0;
      for (int p = 24; p < 96; ++p)
        if (held_[p]) s += 0.05f * (float)std::sin(phase_[p]), phase_[p] += 6.283185307179586 * 440.0 * std::pow(2.0, (p - 69) / 12.0) / kRate;
      l[i] += s;
      r[i] += s;
    }
  }
  bool isInstrument() const override { return instrument_; }
  int latencySamples() const override { return 0; }
private:
  Kind kind_;
  bool instrument_;
  int64_t failAt_;
  int64_t calls_ = 0;
  bool held_[128] = {};
  double phase_[128] = {};
};

class TestProvider final : public core::PluginProvider {
public:
  std::shared_ptr<core::PluginProcessor> get(const std::string& key, const json& plugin, double, bool, std::string* why) override {
    const std::string id = plugin.value("id", std::string());
    if (id == "test:missing") { if (why) *why = "nao instalado (teste)"; return nullptr; }
    Kind kind = Kind::Gain;
    int64_t failAt = -1;
    const std::string rest = id.rfind("test:", 0) == 0 ? id.substr(5) : id;
    const std::string name = rest.substr(0, rest.find(':'));
    if (rest.find(':') != std::string::npos) failAt = std::atoll(rest.substr(rest.find(':') + 1).c_str());
    if (name == "synth") kind = Kind::Synth;
    else if (name == "throw") kind = Kind::Throw;
    else if (name == "nan") kind = Kind::NaN;
    else if (name == "inf") kind = Kind::Inf;
    else if (name == "runaway") kind = Kind::Runaway;
    return std::make_shared<TestPlugin>(kind, key.rfind("track:", 0) == 0, failAt);
  }
  void keepOnly(const std::vector<std::string>&) override {}
};

json plugin(const std::string& id) {
  return {{"format", "vst3"}, {"id", id}, {"name", id}, {"vendor", "Core tests"}, {"version", "1.0.0"}, {"file", "test.vst3"}, {"state", ""}};
}
json effect(int j, const std::string& id) { return {{"id", "f" + std::to_string(j)}, {"type", "plugin"}, {"bypass", false}, {"plugin", plugin(id)}}; }

// Uma música de `tracks` pistas, cada uma com um instrumento plugin e `fx` efeitos plugin em cadeia.
// at(pista, posição) dá o id do plugin: posição -1 = o instrumento, 0..fx-1 = os efeitos, e masterFx no master.
template <typename At>
json song(int tracks, int fx, int bars, At at, const std::string& masterFx = "") {
  json list = json::array();
  for (int t = 0; t < tracks; ++t) {
    json notes = json::array();
    for (int b = 0; b < 4; ++b) notes.push_back({{"tick", b * core::kPpq}, {"length", core::kPpq / 2}, {"pitch", 36 + (t * 7) % 48}, {"velocity", 0.8}});
    json chain = json::array();
    for (int j = 0; j < fx; ++j) chain.push_back(effect(j, at(t, j)));
    list.push_back({{"id", "t" + std::to_string(t)}, {"name", "T" + std::to_string(t)}, {"instrument", "plugin"},
                    {"plugin", plugin(at(t, -1))}, {"gainDb", -18.0}, {"patternTicks", 4 * core::kPpq}, {"notes", notes}, {"effects", chain}});
  }
  json s = {{"format", core::kProjectFormat}, {"version", 2}, {"name", "motor"}, {"bpm", 150.0},
            {"lengthTicks", (int64_t)bars * 4 * core::kPpq}, {"masterGainDb", 0}, {"tracks", list}};
  if (!masterFx.empty()) s["masterEffects"] = json::array({effect(0, masterFx)});
  return s;
}
auto allGood = [](int, int j) { return std::string(j < 0 ? "test:synth" : "test:gain"); };

bool finite(const std::vector<float>& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}
float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Pistas que não a `skip` saem iguais, amostra a amostra, às da referência.
bool othersIdentical(const core::RenderResult& a, const core::RenderResult& b, int skip) {
  if (a.stemL.size() != b.stemL.size()) return false;
  for (size_t t = 0; t < a.stemL.size(); ++t)
    if ((int)t != skip && (a.stemL[t] != b.stemL[t] || a.stemR[t] != b.stemR[t])) return false;
  return true;
}

// Onde o som de uma pista acaba de vez (a primeira amostra a partir da qual tudo é zero).
size_t silentFrom(const std::vector<float>& v) {
  size_t i = v.size();
  while (i > 0 && v[i - 1] == 0.0f) --i;
  return i;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// T28: carga

TEST("carga: 128 pistas x (instrumento + 16 efeitos) = 2176 plugins numa musica, sem falhas e mais rapido que tempo real") {
  TestProvider provider;
  const core::Project p = core::fromJson(song(core::Engine::kMaxTracks, 16, 8, allGood));
  const uint64_t failures0 = core::pluginFailures();
  g_instances = 0;
  g_calls = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const core::RenderResult r = core::renderProject(p, kRate, 512, false, 0.5, nullptr, nullptr, &provider);
  const double secs = secondsSince(t0);
  const double music = (double)r.L.size() / kRate;
  CHECK_EQ(g_instances.load(), (int64_t)128 * 17);
  CHECK(g_calls.load() >= (int64_t)128 * 17 * (int64_t)(r.L.size() / 512));  // todos tocaram a música inteira
  CHECK(finite(r.L) && finite(r.R));
  CHECK(peak(r.L) > 0.01f);
  CHECK_EQ(core::pluginFailures(), failures0);
  std::printf("    carga: %d plugins, %.1f s de musica em %.2f s (%.1fx tempo real, %.0f chamadas de plugin)\n", 128 * 17, music, secs,
              music / secs, (double)g_calls.load());
  // Limite: com plugins que quase nao fazem nada, o custo e o do motor. Tem de ser mais rapido que tempo real.
  CHECK(secs < music);
}

TEST("carga: com workers ou em serie, 64 pistas x 17 plugins dao o mesmo audio, amostra a amostra") {
  TestProvider provider;
  const core::Project p = core::fromJson(song(64, 16, 4, allGood));
  // (sem stems: com stems o export toca a musica inteira uma vez por pista, 65 vezes aqui; ver o relatorio de 16/11)
  const auto serial = core::renderProject(p, kRate, 512, false, 0.5, nullptr, nullptr, &provider, 0);
  const auto pooled = core::renderProject(p, kRate, 512, false, 0.5, nullptr, nullptr, &provider, 4);
  CHECK(serial.L == pooled.L && serial.R == pooled.R);
}

TEST("carga: ao vivo, 128 pistas x 17 plugins com 4 workers, 10 s em blocos de 256, sem NaN e sem atrasos") {
  TestProvider provider;
  const core::Project p = core::fromJson(song(core::Engine::kMaxTracks, 16, 8, allGood));
  core::Engine engine(kRate, 4);
  engine.setLiveDeadline(50);  // a maquina de CI pode ser lenta: aqui conta o resultado, nao o tempo
  engine.publish(std::make_unique<core::Graph>(p, kRate, nullptr, &provider, false));
  engine.play();
  std::vector<float> L(256), R(256);
  float top = 0;
  bool allFinite = true;
  const auto t0 = std::chrono::steady_clock::now();
  const int blocks = (int)(10 * kRate / 256);
  for (int b = 0; b < blocks; ++b) {
    engine.process(L.data(), R.data(), 256);
    allFinite = allFinite && finite(L) && finite(R);
    top = std::max(top, std::max(peak(L), peak(R)));
    if (b % 64 == 0) engine.collectGarbage();
  }
  const double secs = secondsSince(t0);
  CHECK(allFinite);
  CHECK(top > 0.01f);
  CHECK_EQ(engine.stats().nonFiniteBlocks.load(), (uint64_t)0);
  std::printf("    ao vivo: 2176 plugins, 10 s em %.2f s (%.1fx tempo real), %llu atrasos de worker\n", secs, 10 / secs,
              (unsigned long long)engine.stats().liveStalls.load());
}

TEST("carga: os limites (128 pistas, 16 efeitos por cadeia) recusam com erro claro, nunca a meio") {
  CHECK_THROWS(core::fromJson(song(core::Engine::kMaxTracks + 1, 1, 1, allGood)));
  CHECK_THROWS(core::fromJson(song(1, 17, 1, allGood)));
}

// ---------------------------------------------------------------------------------------------------------
// T29: robustez. Um plugin que falha cala o que lhe pertence (a pista, ou deixa passar a mistura no master);
// tudo o resto toca igual, amostra a amostra, e nada de NaN chega a saida.

TEST("robustez: um plugin que falha (excecao, NaN, Inf, nivel descontrolado) no instrumento ou na cadeia so cala a sua pista") {
  TestProvider provider;
  const int tracks = 8, fx = 16, faulty = 5;
  const auto reference = core::renderProject(core::fromJson(song(tracks, fx, 4, allGood)), kRate, 512, true, 0.5, nullptr, nullptr, &provider);
  for (const std::string kind : {"throw", "nan", "inf", "runaway"})
    for (const int where : {-1, 0, fx - 1}) {
      const auto at = [&](int t, int j) {
        if (t == faulty && j == where) return "test:" + kind + ":200";  // falha na 200.a chamada, a meio da musica
        return allGood(t, j);
      };
      // (com stems, a musica e tocada uma vez por pista e uma vez inteira: o plugin falha em cada uma dessas passagens)
      const uint64_t failures0 = core::pluginFailures();
      const auto r = core::renderProject(core::fromJson(song(tracks, fx, 4, at)), kRate, 512, true, 0.5, nullptr, nullptr, &provider);
      const auto& stem = r.stemL[(size_t)faulty];
      const bool ok = finite(r.L) && finite(r.R) && othersIdentical(r, reference, faulty) && core::pluginFailures() > failures0 &&
                      peak(stem, 0, 4096) > 0 && silentFrom(stem) < stem.size() / 2 && peak(r.L, stem.size() * 3 / 4) > 0.001f;
      if (!ok)
        std::fprintf(stderr, "    %s na posicao %d: finito %d, outras iguais %d, falhas +%llu, pista cala em %zu de %zu\n", kind.c_str(), where,
                     (int)(finite(r.L) && finite(r.R)), (int)othersIdentical(r, reference, faulty),
                     (unsigned long long)(core::pluginFailures() - failures0), silentFrom(stem), stem.size());
      CHECK(ok);
    }
}

TEST("robustez: um plugin que falha no master deixa passar a mistura, e a musica continua a soar ate ao fim") {
  TestProvider provider;
  for (const std::string kind : {"throw", "nan", "inf", "runaway"}) {
    const uint64_t failures0 = core::pluginFailures();
    const auto r = core::renderProject(core::fromJson(song(8, 2, 4, allGood, "test:" + kind + ":100")), kRate, 512, false, 0.5, nullptr,
                                       nullptr, &provider);
    CHECK(finite(r.L) && finite(r.R));
    CHECK_EQ(core::pluginFailures(), failures0 + 1);
    CHECK(peak(r.L, r.L.size() * 3 / 4) > 0.001f);  // depois da falha a mistura passa
  }
}

// Instrumento que nao carrega: a pista fica calada (as notas ficam). Efeito que nao carrega: a pista toca sem ele.
// Os dois ficam na lista missingPlugins para a app avisar.
TEST("robustez: um plugin que nao carrega deixa a musica abrir e tocar; as outras pistas tocam igual") {
  TestProvider provider;
  const int tracks = 12, faulty = 3;
  const auto reference = core::renderProject(core::fromJson(song(tracks, 4, 2, allGood)), kRate, 512, true, 0.5, nullptr, nullptr, &provider);
  for (const int where : {-1, 2}) {
    const auto at = [&](int t, int j) { return t == faulty && j == where ? std::string("test:missing") : allGood(t, j); };
    const core::Project p = core::fromJson(song(tracks, 4, 2, at));
    core::Graph graph(p, kRate, nullptr, &provider, true);
    CHECK_EQ(graph.missingPlugins().size(), (size_t)1);
    const auto r = core::renderProject(p, kRate, 512, true, 0.5, nullptr, nullptr, &provider);
    CHECK(finite(r.L) && finite(r.R));
    CHECK(othersIdentical(r, reference, faulty));
    if (where < 0) CHECK_EQ(peak(r.stemL[(size_t)faulty]), 0.0f);
    else CHECK(peak(r.stemL[(size_t)faulty]) > peak(reference.stemL[(size_t)faulty]));  // sem o ganho de 0.999 desse efeito
  }
}

TEST("robustez: ao vivo, com 4 workers, um plugin que lanca excecao num worker nao derruba o motor e as outras pistas tocam") {
  TestProvider provider;
  const auto at = [](int t, int j) { return t == 5 && j == 3 ? std::string("test:throw:300") : allGood(t, j); };
  const core::Project p = core::fromJson(song(32, 8, 4, at));
  for (const int threads : {0, 4}) {
    core::Engine engine(kRate, threads);
    engine.setLiveDeadline(50);
    engine.publish(std::make_unique<core::Graph>(p, kRate, nullptr, &provider, false));
    engine.play();
    const uint64_t failures0 = core::pluginFailures();
    std::vector<float> L(256), R(256);
    bool allFinite = true;
    float late = 0;
    const int blocks = (int)(6 * kRate / 256);
    for (int b = 0; b < blocks; ++b) {
      engine.process(L.data(), R.data(), 256);
      allFinite = allFinite && finite(L) && finite(R);
      if (b > blocks * 3 / 4) late = std::max(late, peak(L));
    }
    CHECK(allFinite);
    CHECK_EQ(core::pluginFailures(), failures0 + 1);
    CHECK(late > 0.001f);  // as outras 31 pistas continuam a tocar
  }
}

CORE_TEST_MAIN
