// F1 Freeze (Diogo 2026-11-16, "Tornar o Core leve e fiável"): a spec da fase 2 do plano, verificada no motor e no
// documento.
//   - render pré-fader: congelado == vivo (null test) com fader, pan, bus e um plugin com latência na cadeia;
//   - PDC: um impulso através de um plugin com latência sai no sítio (offset 0);
//   - caudas: o que soa depois do fim do clip fica no áudio congelado;
//   - o mixer fica vivo: fader, pan, sends, saída e a automação "gainDb"/"pan" passam para a pista de áudio e voltam
//     no Unfreeze, com o que se mexeu entretanto;
//   - editar o MIDI (ou o plugin, o tempo, o comprimento) invalida o freeze explicitamente, no mesmo passo de Undo;
//   - sidechain: a pista que é chave de outra é recusada com mensagem clara; uma pista com chave congela com a chave;
//   - um só Undo para Freeze e para Unfreeze; guardar e abrir mantém tudo.
// Correr: freeze_tests [parte-do-nome]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include "support/check.hpp"
#include "support/signals.hpp"
#include "edit.hpp"
#include "history.hpp"
#include "quick_tools.hpp"
#include "core/project.hpp"
#include "core/server.hpp"
#include "io/wav.hpp"

using namespace core::doc;
namespace fs = std::filesystem;
using core::json;

namespace {
constexpr double kSr = 48000;
constexpr int64_t kBar = 4 * kPpq;

fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / "core-freeze-tests" / name;
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// ---- motor: um plugin falso com latência (um atraso puro que o diz) e um que divide o nível por 2

class DelayPlugin final : public core::PluginProcessor {
public:
  explicit DelayPlugin(int latency) : l_((size_t)latency, 0.0f), r_((size_t)latency, 0.0f) {}
  void process(float* left, float* right, int frames, const core::MidiNote*, int) override {
    if (l_.empty()) return;
    for (int i = 0; i < frames; ++i) {
      std::swap(left[i], l_[at_]);
      std::swap(right[i], r_[at_]);
      if (++at_ == l_.size()) at_ = 0;
    }
  }
  bool isInstrument() const override { return false; }
  int latencySamples() const override { return (int)l_.size(); }
private:
  std::vector<float> l_, r_;
  size_t at_ = 0;
};

class SoftClipPlugin final : public core::PluginProcessor {  // não linear: o nível de entrada conta
public:
  void process(float* left, float* right, int frames, const core::MidiNote*, int) override {
    for (int i = 0; i < frames; ++i) { left[i] = std::tanh(2.0f * left[i]); right[i] = std::tanh(2.0f * right[i]); }
  }
  bool isInstrument() const override { return false; }
  int latencySamples() const override { return 0; }
};

class FakeHost final : public core::PluginHost {
public:
  std::shared_ptr<core::PluginProcessor> get(const std::string&, const json& plugin, double, bool, std::string* why) override {
    const std::string id = plugin.value("id", "");
    if (id == "fake-delay-300") return std::make_shared<DelayPlugin>(300);
    if (id == "fake-softclip") return std::make_shared<SoftClipPlugin>();
    if (why) *why = "unknown fake plugin";
    return nullptr;
  }
  void keepOnly(const std::vector<std::string>&) override {}
  json captureStates() override { return json::object(); }
  bool showEditor(const std::string&, bool, std::string*) override { return false; }
  bool takeChanged() override { return false; }
  void releaseAll() override {}
};

json fx(const std::string& id, const std::string& plugin) {
  return {{"type", "plugin"}, {"id", id}, {"plugin", {{"format", "vst3"}, {"id", plugin}, {"name", plugin}}}};
}

fs::path wavOf(const fs::path& file, const std::vector<float>& L, const std::vector<float>& R) {
  core::writeWav(file.string(), L, R, (int)kSr, 32);
  return file;
}

json audioClip(const fs::path& file, double seconds, int64_t startTick = 0) {
  return {{"startTick", startTick}, {"path", file.string()}, {"offsetSeconds", 0}, {"durationSeconds", seconds}, {"gainDb", 0}};
}

json engineSong(const json& tracks) {
  return {{"format", core::kProjectFormat}, {"version", 2}, {"name", "freeze"}, {"bpm", 150.0},
          {"lengthTicks", 4 * kBar}, {"masterGainDb", -2.0}, {"tracks", tracks}};
}

// Um render.start completo (como a app pede), à espera do fim com teto: falha alto se não acabar.
void render(core::Server& server, const json& files, const std::string& what) {
  json r = server.handle({{"cmd", "render.start"}, {"args", {{"files", files}, {"bits", 32}, {"tailMs", 500}}}});
  if (!r.value("ok", false)) { coretest::fail(__FILE__, __LINE__, what + ": render.start recusado: " + r.dump()); return; }
  const auto job = r["result"]["jobId"];
  std::string state;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < until) {
    state = server.handle({{"cmd", "render.status"}, {"args", {{"jobId", job}}}})["result"].value("state", "");
    if (state == "succeeded" || state == "failed" || state == "cancelled") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (state != "succeeded") coretest::fail(__FILE__, __LINE__, what + ": o render acabou em '" + state + "'");
}

void load(core::Server& server, const json& song) {
  json r = server.handle({{"cmd", "project.load"}, {"args", {{"project", song}}}});
  if (!r.value("ok", false)) coretest::fail(__FILE__, __LINE__, "project.load: " + r.dump());
}

double maxDiff(const core::AudioData& a, const core::AudioData& b) {
  if (a.frames() != b.frames()) return 1e9;
  double worst = 0;
  for (size_t i = 0; i < a.L.size(); ++i)
    worst = std::max({worst, (double)std::fabs(a.L[i] - b.L[i]), (double)std::fabs(a.R[i] - b.R[i])});
  return worst;
}

// ---- documento

json plugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", name + "-id"}, {"name", name}, {"vendor", "Xfer"}, {"file", name + ".vst3"}};
}

fs::path renderFile(const fs::path& dir) {
  const auto L = coretest::sine(110, 48000, 96000, 0.5f);
  const fs::path f = dir / "render.wav";
  core::writeWav(f.string(), L, L, 48000, 24);
  return f;
}

std::string bassTrack(Document& d) {
  const std::string id = addTrack(d, "", "Bass");
  replaceInstrumentWithPlugin(d, id, plugin("Serum 2"));
  Track& t = trackById(d, id);
  t.clips.clear();
  Pattern& p = t.patterns.front();
  p.lengthTicks = kBar;
  p.notes.clear();
  for (int i = 0; i < 4; ++i) p.notes.push_back(Note{"n" + std::to_string(i + 1), i * kPpq, kPpq / 4, 36, 0.8});
  addClip(t, p.id, 0, d.lengthTicks);
  return id;
}

std::string busTrack(Document& d) {
  const std::string id = addTrack(d, "", "Bus");
  Track& b = trackById(d, id);
  b.instrument = Instrument{};
  b.instrument.kind = "bus";
  b.clips.clear();
  return id;
}
}  // namespace

// ================================================================== motor

TEST("freeze no motor: congelado == vivo (fader, pan, bus, master, plugin com latência e não linear)") {
  const fs::path dir = freshDir("null");
  const auto noiseL = coretest::noise(48000, 11, 0.4f), noiseR = coretest::noise(48000, 12, 0.4f);
  const fs::path src = wavOf(dir / "src.wav", noiseL, noiseR);
  const auto other = coretest::sine(330, kSr, 48000, 0.3f);
  const fs::path oth = wavOf(dir / "other.wav", other, other);
  // A: áudio -> softclip -> atraso de 300 amostras; fader -7 dB, pan 0,35, para o bus (-3 dB); C: outra pista direta.
  json a = {{"id", "A"}, {"name", "A"}, {"instrument", "audio"}, {"gainDb", -7.0}, {"pan", 0.35}, {"output", "B"},
            {"audioClips", json::array({audioClip(src, 0.9)})}, {"effects", json::array({fx("e1", "fake-softclip"), fx("e2", "fake-delay-300")})}};
  json b = {{"id", "B"}, {"name", "Bus"}, {"instrument", "bus"}, {"gainDb", -3.0}};
  json c = {{"id", "C"}, {"name", "C"}, {"instrument", "audio"}, {"gainDb", -1.0}, {"audioClips", json::array({audioClip(oth, 0.9)})}};
  FakeHost host;
  core::Server server(nullptr, kSr, &host);
  load(server, engineSong(json::array({a, b, c})));
  render(server, json::array({{{"path", (dir / "live.wav").string()}}}), "vivo");
  render(server, json::array({{{"path", (dir / "stem.wav").string()}, {"track", "A"}, {"preFader", true}}}), "stem pré-fader");
  // O congelado: A passa a tocar o stem, sem plugins, com o mesmo mixer (fader, pan, saída).
  json frozen = a;
  frozen["effects"] = json::array();
  frozen["audioClips"] = json::array({audioClip(dir / "stem.wav", 60)});
  load(server, engineSong(json::array({frozen, b, c})));
  render(server, json::array({{{"path", (dir / "frozen.wav").string()}}}), "congelado");
  const auto live = core::readWav((dir / "live.wav").string(), 1 << 24);
  const auto froz = core::readWav((dir / "frozen.wav").string(), 1 << 24);
  const double diff = maxDiff(live, froz);
  // Float de 32 bits dos dois lados: a única diferença é o arredondamento de um ganho aplicado noutra ordem.
  if (!(diff < 1e-5)) coretest::fail(__FILE__, __LINE__, coretest::pair(diff, "< 1e-5 (null test)"));
  CHECK(live.frames() > 0);
}

TEST("freeze no motor: PDC, um impulso através de um plugin com latência sai no sítio (offset 0)") {
  const fs::path dir = freshDir("pdc");
  std::vector<float> imp(24000, 0.0f);
  const int64_t at = 12345;
  imp[(size_t)at] = 0.5f;
  const fs::path src = wavOf(dir / "imp.wav", imp, imp);
  json a = {{"id", "A"}, {"name", "A"}, {"instrument", "audio"}, {"gainDb", -12.0}, {"pan", -0.8},
            {"audioClips", json::array({audioClip(src, 0.5)})}, {"effects", json::array({fx("d", "fake-delay-300")})}};
  FakeHost host;
  core::Server server(nullptr, kSr, &host);
  load(server, engineSong(json::array({a})));
  render(server, json::array({{{"path", (dir / "stem.wav").string()}, {"track", "A"}, {"preFader", true}}}), "stem");
  const auto s = core::readWav((dir / "stem.wav").string(), 1 << 24);
  int64_t peakAt = -1;
  float peak = 0;
  for (size_t i = 0; i < s.L.size(); ++i)
    if (std::fabs(s.L[i]) > peak) { peak = std::fabs(s.L[i]); peakAt = (int64_t)i; }
  CHECK_EQ(peakAt, at);
  // Pré-fader: nem o fader de -12 dB nem o pan a -0,8 estão no áudio (eles ficam vivos na pista congelada).
  CHECK_NEAR(peak, 0.5, 1e-6);
  CHECK_NEAR(std::fabs(s.R[(size_t)at]), 0.5, 1e-6);
}

TEST("freeze no motor: a cauda depois do fim do clip fica no áudio congelado") {
  const fs::path dir = freshDir("tail");
  const auto tone = coretest::sine(200, kSr, 4800, 0.5f);  // 0,1 s
  const fs::path src = wavOf(dir / "tone.wav", tone, tone);
  json a = {{"id", "A"}, {"name", "A"}, {"instrument", "audio"},
            {"audioClips", json::array({audioClip(src, 0.1)})}, {"effects", json::array({fx("d", "fake-delay-300")})}};
  FakeHost host;
  core::Server server(nullptr, kSr, &host);
  load(server, engineSong(json::array({a})));
  render(server, json::array({{{"path", (dir / "stem.wav").string()}, {"track", "A"}, {"preFader", true}}}), "stem");
  const auto s = core::readWav((dir / "stem.wav").string(), 1 << 24);
  // O clip acaba na amostra 4800; um atraso compensado devolve o som no sítio: nada se perde no fim.
  float lastQuarter = 0;
  for (size_t i = 3600; i < 4800; ++i) lastQuarter = std::max(lastQuarter, std::fabs(s.L[i]));
  CHECK(lastQuarter > 0.4f);
}

TEST("freeze no motor: uma pista que é chave de sidechain continua a tocar a chave no render pré-fader de outra") {
  // O render pré-fader cala todas as outras pistas mas deixa-as tocar: a chave de um sidechain continua lá. Aqui sem
  // compressor nativo (o Core já não tem som próprio): verifica-se que o stem só tem A, nada de C nem do bus.
  const fs::path dir = freshDir("only-a");
  const auto toneA = coretest::sine(200, kSr, 48000, 0.25f), toneC = coretest::sine(900, kSr, 48000, 0.25f);
  json a = {{"id", "A"}, {"name", "A"}, {"instrument", "audio"}, {"output", "B"}, {"sends", json::array({{{"to", "B"}, {"gainDb", 0}}})},
            {"audioClips", json::array({audioClip(wavOf(dir / "a.wav", toneA, toneA), 0.9)})}};
  json b = {{"id", "B"}, {"name", "Bus"}, {"instrument", "bus"}};
  json c = {{"id", "C"}, {"name", "C"}, {"instrument", "audio"}, {"audioClips", json::array({audioClip(wavOf(dir / "c.wav", toneC, toneC), 0.9)})}};
  FakeHost host;
  core::Server server(nullptr, kSr, &host);
  load(server, engineSong(json::array({a, b, c})));
  render(server, json::array({{{"path", (dir / "stem.wav").string()}, {"track", "A"}, {"preFader", true}}}), "stem");
  const auto s = core::readWav((dir / "stem.wav").string(), 1 << 24);
  double worst = 0;
  for (size_t i = 0; i < 40000; ++i) worst = std::max(worst, (double)std::fabs(s.L[i] - toneA[i]));
  if (!(worst < 1e-6)) coretest::fail(__FILE__, __LINE__, coretest::pair(worst, "< 1e-6 (só A, uma vez, sem send nem bus)"));
}

// ================================================================== documento

TEST("freeze: o mixer passa para a pista de áudio e fica vivo (fader, pan, saída, sends, automação de mixer)") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const std::string bus = busTrack(d);
  Track& t = trackById(d, bass);
  t.mix.gainDb = -4.5;
  t.mix.pan = 0.25;
  t.mix.output = bus;
  t.automation.push_back(Automation{"gainDb", {{0, -10}, {kBar, -2}}});
  t.automation.push_back(Automation{"pan", {{0, -0.5}, {kBar, 0.5}}});
  t.automation.push_back(Automation{"cutoff", {{0, 0.1}, {kBar, 0.9}}});  // do plugin: fica na fonte (baked)
  const fs::path dir = freshDir("mixer");
  const Printed p = freezeTrack(d, bass, renderFile(dir), dir);
  const Track& audio = *d.findTrack(p.trackId);
  const Track& src = *d.findTrack(bass);
  CHECK_NEAR(audio.mix.gainDb, -4.5, 0);
  CHECK_NEAR(audio.mix.pan, 0.25, 0);
  CHECK_EQ(audio.mix.output, bus);
  CHECK(!audio.mix.mute);
  CHECK_EQ(audio.automation.size(), (size_t)2);
  CHECK_EQ(src.automation.size(), (size_t)1);
  CHECK_EQ(src.automation.front().param, std::string("cutoff"));
  // Mexer no fader da pista congelada não invalida o freeze (o render é pré-fader).
  trackById(d, p.trackId).mix.gainDb = -9;
  CHECK(unfreezeStale(d).empty());
  CHECK(isFrozen(*d.findTrack(bass)));
  // Unfreeze: a fonte volta com o mixer que a pista de áudio tinha, incluindo o que mudou entretanto.
  unfreezeTrack(d, bass);
  const Track& back = *d.findTrack(bass);
  CHECK_NEAR(back.mix.gainDb, -9, 0);
  CHECK_NEAR(back.mix.pan, 0.25, 0);
  CHECK_EQ(back.mix.output, bus);
  CHECK(!back.mix.mute);
  CHECK_EQ(back.automation.size(), (size_t)3);
  CHECK(!d.findTrack(p.trackId));
}

TEST("freeze: editar o MIDI invalida o freeze explicitamente, no mesmo passo de Undo") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("stale");
  History h(d);
  Document frozen = h.current();
  const Printed p = freezeTrack(frozen, bass, renderFile(dir), dir);
  h.commit(frozen, "Freeze Bass");
  CHECK(isFrozen(*h.current().findTrack(bass)));
  CHECK(h.unfrozenByLastChange().empty());
  // Uma nota mexida: o áudio congelado já não é esta pista.
  Document edited = h.current();
  trackById(edited, bass).patterns.front().notes.front().pitch = 41;
  h.commit(edited, "Move note");
  CHECK(!isFrozen(*h.current().findTrack(bass)));
  CHECK(!h.current().findTrack(p.trackId));
  CHECK_EQ(h.unfrozenByLastChange().size(), (size_t)1);
  CHECK_EQ(h.current().findTrack(bass)->patterns.front().notes.front().pitch, 41);
  CHECK(!h.current().findTrack(bass)->mix.mute);
  // Um Undo: a nota e o freeze voltam juntos.
  CHECK(h.undo());
  CHECK(isFrozen(*h.current().findTrack(bass)));
  CHECK(h.current().findTrack(p.trackId) != nullptr);
  CHECK_EQ(h.current().findTrack(bass)->patterns.front().notes.front().pitch, 36);
  // O tempo também invalida; mexer noutra pista ou no loop não.
  Document other = h.current();
  other.loop.enabled = !other.loop.enabled;
  h.commit(other, "Loop");
  CHECK(isFrozen(*h.current().findTrack(bass)));
  Document faster = h.current();
  faster.bpm += 10;
  h.commit(faster, "Tempo");
  CHECK(!isFrozen(*h.current().findTrack(bass)));
}

TEST("freeze: um gesto (arrastar) que muda a pista também a descongela") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("gesture");
  History h(d);
  Document frozen = h.current();
  freezeTrack(frozen, bass, renderFile(dir), dir);
  h.commit(frozen, "Freeze Bass");
  h.beginGesture("Drag note");
  Document drag = h.current();
  trackById(drag, bass).patterns.front().notes.front().tick += 24;
  h.updateGesture(drag);
  h.endGesture();
  CHECK(!isFrozen(*h.current().findTrack(bass)));
  CHECK(h.undo());
  CHECK(isFrozen(*h.current().findTrack(bass)));
}

TEST("freeze: Freeze e Unfreeze são um só passo de Undo cada; guardar e abrir mantém a impressão") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("undo");
  History h(d);
  Document frozen = h.current();
  const Printed p = freezeTrack(frozen, bass, renderFile(dir), dir);
  h.commit(frozen, "Freeze Bass");
  Document reopened = parseDocument(toJson(h.current()));
  CHECK(unfreezeStale(reopened).empty());  // a impressão sobrevive a guardar/abrir
  CHECK(isFrozen(*reopened.findTrack(bass)));
  Document thawed = h.current();
  unfreezeTrack(thawed, bass);
  h.commit(thawed, "Unfreeze Bass");
  CHECK(!h.current().findTrack(p.trackId));
  CHECK(h.undo());
  CHECK(isFrozen(*h.current().findTrack(bass)));
  CHECK(h.undo());
  CHECK(!isFrozen(*h.current().findTrack(bass)));
  CHECK(!h.current().findTrack(p.trackId));
  CHECK(!h.canUndo());
}

TEST("freeze: a chave de um sidechain é recusada com mensagem clara; a pista com chave congela") {
  Document d = newEmptyDocument();
  const std::string kick = bassTrack(d);
  const std::string bass = bassTrack(d);
  trackById(d, kick).name = "Kick";
  // Um compressor na Bass com chave no Kick (como o documento o guarda).
  trackById(d, bass).effects.push_back({{"type", "builtin"}, {"name", "compressor"}, {"id", "comp"}, {"sidechain", kick}});
  const std::string why = cannotFreeze(d, kick);
  CHECK(why.find("sidechain") != std::string::npos);
  CHECK(why.find("Bass") != std::string::npos);
  const fs::path dir = freshDir("sidechain");
  CHECK_THROWS(freezeTrack(d, kick, renderFile(dir), dir));
  // A Bass (com chave) congela; mexer nas notas do Kick muda o que ela ouvia: invalida.
  CHECK_EQ(cannotFreeze(d, bass), std::string());
  freezeTrack(d, bass, renderFile(dir), dir);
  CHECK(isFrozen(*d.findTrack(bass)));
  trackById(d, kick).patterns.front().notes.front().pitch = 40;
  CHECK_EQ(unfreezeStale(d).size(), (size_t)1);
  CHECK(!isFrozen(*d.findTrack(bass)));
}

CORE_TEST_MAIN
