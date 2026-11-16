// Testes das QoL vindas de outras DAWs (Diogo 2026-11-16, "aplicar as QoL do estudo"; quick_tools.hpp):
// secções como marcadores, Double e shift de linha no Patterns, nudge de clips e notas, Freeze / Unfreeze.
#include <filesystem>
#include "support/check.hpp"
#include "support/signals.hpp"
#include "assets.hpp"
#include "bitwig.hpp"
#include "edit.hpp"
#include "midi.hpp"
#include "pattern_rack.hpp"
#include "quick_tools.hpp"
#include "core/project.hpp"
#include "io/wav.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kBar = 4 * kPpq, kStep = kPpq / 4;

json plugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", name + "-id"}, {"name", name}, {"vendor", "Xfer"}, {"file", name + ".vst3"}};
}

fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / "core-quick-tools-tests" / name;
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// A render as the engine would leave it: 2 s of a sine.
fs::path renderFile(const fs::path& dir) {
  const auto L = coretest::sine(110, 48000, 96000, 0.5f);
  const fs::path f = dir / "render.wav";
  core::writeWav(f.string(), L, L, 48000, 24);
  return f;
}

Document songWithSections() {
  Document d = newEmptyDocument();
  d.lengthTicks = 32 * kBar;
  addSection(d, "Intro", 0, 8 * kBar);
  addSection(d, "Build", 8 * kBar, 8 * kBar);
  addSection(d, "Drop", 16 * kBar, 16 * kBar);
  return d;
}

// A Serum 2 bass with notes and one clip over the whole song.
std::string bassTrack(Document& d) {
  const std::string id = addTrack(d, "", "Bass");
  replaceInstrumentWithPlugin(d, id, plugin("Serum 2"));
  Track& t = trackById(d, id);
  t.clips.clear();
  const std::string pid = t.patterns.front().id;
  Pattern& p = patternById(t, pid);
  p.lengthTicks = kBar;
  p.notes.clear();
  for (int i = 0; i < 4; ++i) p.notes.push_back(Note{"n" + std::to_string(i + 1), i * kPpq + kStep, kStep, 36, 0.8});
  addClip(t, pid, 0, d.lengthTicks);
  return id;
}
}  // namespace

// ---- secções como marcadores

TEST("secções: a seguinte e a anterior a partir de onde se está") {
  const Document d = songWithSections();
  CHECK_EQ(nextSectionStart(d, 0), 8 * kBar);
  CHECK_EQ(nextSectionStart(d, 10 * kBar), 16 * kBar);
  CHECK_EQ(nextSectionStart(d, 16 * kBar), (int64_t)-1);  // depois do Drop não há mais
  CHECK_EQ(previousSectionStart(d, 10 * kBar), 8 * kBar);
  CHECK_EQ(previousSectionStart(d, 8 * kBar), (int64_t)0);  // no início do Build, a anterior é a Intro
  CHECK_EQ(previousSectionStart(d, 0), (int64_t)-1);
  // A tocar: carregar logo depois do início volta à anterior, não ao início desta.
  CHECK_EQ(previousSectionStart(d, 8 * kBar + kStep, kBar / 2), (int64_t)0);
}

TEST("secções: o loop de uma secção é o seu tempo, cortado no fim da música") {
  Document d = songWithSections();
  const std::string build = sectionAt(d, 9 * kBar)->id;
  const Loop l = sectionLoop(d, build);
  CHECK(l.enabled);
  CHECK_EQ(l.startTick, 8 * kBar);
  CHECK_EQ(l.endTick, 16 * kBar);
  d.lengthTicks = 20 * kBar;
  const Loop drop = sectionLoop(d, sectionAt(d, 17 * kBar)->id);
  CHECK_EQ(drop.endTick, 20 * kBar);
  CHECK_THROWS(sectionLoop(d, "nao-existe"));
  CHECK(sectionAt(d, 40 * kBar) == nullptr);
}

TEST("secções: vão no .mid como marcadores (para o Bitwig) e voltam a ler-se") {
  Document d = songWithSections();
  bassTrack(d);
  const MidiFile back = parseMidi(exportMidi(d));
  CHECK_EQ(back.markers.size(), (size_t)3);
  if (back.markers.size() == 3) {
    CHECK_EQ(back.markers[0].text, std::string("Intro"));
    CHECK_EQ(back.markers[1].tick, 8 * kBar);
    CHECK_EQ(back.markers[2].text, std::string("Drop"));
  }
  CHECK_EQ(back.tracks.size(), (size_t)1);  // os marcadores não criam pistas
}

// ---- Patterns: Double e shift de linha

TEST("Double: o pattern passa ao dobro com as notas repetidas; a 8 compassos recusa") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const std::string slot = addRackSlot(d, "Main Groove", 1);
  Pattern& p = rackPatternFor(d, slot, bass);
  p.notes = {Note{"n1", kStep, kStep, 36, 0.8}, Note{"n2", kBar - kStep, kStep, 38, 0.8}};
  CHECK_EQ(doubleRackSlot(d, slot), 2);
  const Pattern& p2 = *rackPattern(d, *findRackSlot(rackSlots(d), slot), bass);
  CHECK_EQ(p2.lengthTicks, 2 * kBar);
  CHECK_EQ(p2.notes.size(), (size_t)4);
  CHECK_EQ(doubleRackSlot(d, slot), 4);
  CHECK_EQ(doubleRackSlot(d, slot), 8);
  const std::string before = toJson(d).dump();
  CHECK_THROWS(doubleRackSlot(d, slot));
  CHECK_EQ(toJson(d).dump(), before);  // recusado: nada muda
  CHECK_THROWS(doubleRackSlot(d, "nao-existe"));
}

TEST("shift de linha: os steps andam 1/16 e o que sai de um lado volta pelo outro") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const std::string slot = addRackSlot(d, "Main Groove", 1);
  Pattern& p = rackPatternFor(d, slot, bass);
  p.notes = {Note{"n1", 0, kStep, 36, 0.8}, Note{"n2", kBar - kStep, kStep, 36, 0.8}};
  CHECK_EQ(shiftRackRow(d, slot, bass, 1), 2);
  const Pattern& after = *rackPattern(d, *findRackSlot(rackSlots(d), slot), bass);
  CHECK_EQ(after.notes[0].tick, (int64_t)0);  // o último 1/16 deu a volta para o início
  CHECK_EQ(after.notes[0].id, std::string("n2"));
  CHECK_EQ(after.notes[1].tick, kStep);
  CHECK_EQ(shiftRackRow(d, slot, bass, -1), 2);  // e volta atrás
  const Pattern& back = *rackPattern(d, *findRackSlot(rackSlots(d), slot), bass);
  CHECK_EQ(back.notes[0].tick, (int64_t)0);
  CHECK_EQ(back.notes[0].id, std::string("n1"));
  CHECK_EQ(back.notes[1].tick, kBar - kStep);
  CHECK_EQ(shiftRackRow(d, slot, bass, 0), 0);
  CHECK_THROWS(shiftRackRow(d, slot, "nao-existe", 1));
}

TEST("shift de linha: uma linha vazia não muda nada; só as notas escolhidas rodam no piano roll") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const std::string slot = addRackSlot(d, "Vazio", 1);
  CHECK_EQ(shiftRackRow(d, slot, bass, 1), 0);
  Pattern p;
  p.lengthTicks = kBar;
  p.notes = {Note{"a", 0, kStep, 36, 0.8}, Note{"b", kPpq, kStep, 36, 0.8}};
  CHECK_EQ(rotatePatternNotes(p, -kStep, {"a"}), 1);
  CHECK_EQ(p.notes.back().id, std::string("a"));
  CHECK_EQ(p.notes.back().tick, kBar - kStep);
  CHECK_EQ(p.notes.front().tick, kPpq);  // "b" não se mexeu
}

// ---- nudge

TEST("nudge: clips de padrão e de áudio andam juntos um passo; a música cresce se for preciso") {
  Document d = newEmptyDocument();
  d.lengthTicks = 4 * kBar;
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("nudge");
  const std::string asset = importAudio(d, renderFile(dir), dir);
  const std::string vox = addAudioTrack(d, "Vox");
  const std::string ac = addAudioClip(d, vox, asset, kBar);
  const std::string clip = trackById(d, bass).clips.front().id;
  CHECK_EQ(nudgeClips(d, {{bass, clip, false}, {vox, ac, true}}, kStep), 2);
  CHECK_EQ(trackById(d, bass).clips.front().startTick, kStep);
  CHECK_EQ(trackById(d, vox).audioClips.front().startTick, kBar + kStep);
  CHECK_EQ(d.lengthTicks, 5 * kBar);  // o clip de 4 compassos acaba depois do fim: a música cresce até ao compasso
  CHECK_EQ(nudgeClips(d, {{bass, clip, false}, {vox, ac, true}}, -kStep), 2);
  CHECK_EQ(trackById(d, bass).clips.front().startTick, (int64_t)0);
}

TEST("nudge: se um clip sairia antes do início, nenhum se mexe") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const std::string other = addTrack(d, "", "Lead");
  Track& t = trackById(d, other);
  t.clips.clear();
  const std::string c2 = addClip(t, t.patterns.front().id, 2 * kBar, kBar);
  const std::string c1 = trackById(d, bass).clips.front().id;  // começa no 0
  const std::string before = toJson(d).dump();
  CHECK_THROWS(nudgeClips(d, {{other, c2, false}, {bass, c1, false}}, -kStep));
  CHECK_EQ(toJson(d).dump(), before);
  CHECK_THROWS(nudgeClips(d, {{bass, "nao-existe", false}}, kStep));
  CHECK_EQ(nudgeClips(d, {}, kStep), 0);
}

TEST("nudge de notas: andam dentro do padrão; se uma sairia, nenhuma se mexe") {
  Pattern p;
  p.lengthTicks = kBar;
  p.notes = {Note{"a", 0, kStep, 36, 0.8}, Note{"b", kPpq, kStep, 38, 0.8}};
  CHECK_EQ(nudgeNotes(p, {"b"}, kStep), 1);
  CHECK_EQ(p.notes[1].tick, kPpq + kStep);
  CHECK_THROWS(nudgeNotes(p, {"a", "b"}, -kStep));  // "a" sairia antes do início
  CHECK_EQ(p.notes[0].tick, (int64_t)0);
  CHECK_EQ(p.notes[1].tick, kPpq + kStep);
  CHECK_THROWS(nudgeNotes(p, {"b"}, kBar));
}

// ---- freeze

TEST("freeze: o áudio vai para uma pista por baixo e o motor deixa de receber os plugins da original") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  addPluginEffect(d, bass, plugin("Saturn 2"));
  const fs::path dir = freshDir("freeze");
  CHECK_EQ(cannotFreeze(d, bass), std::string());
  const Printed p = freezeTrack(d, bass, renderFile(dir), dir);
  const Track& src = *d.findTrack(bass);
  const Track& audio = *d.findTrack(p.trackId);
  CHECK(isFrozen(src));
  CHECK_EQ(frozenInto(src), p.trackId);
  CHECK(src.mix.mute);
  CHECK_EQ(audio.name, std::string("Bass (frozen)"));
  CHECK_EQ(audio.instrument.kind, std::string("audio"));
  // Logo por baixo da original.
  size_t si = 0, ai = 0;
  for (size_t i = 0; i < d.tracks.size(); ++i) { if (d.tracks[i].id == bass) si = i; if (d.tracks[i].id == p.trackId) ai = i; }
  CHECK_EQ(ai, si + 1);
  // As notas e os plugins ficam no documento; o motor recebe a pista sem eles.
  CHECK_EQ(src.instrument.kind, std::string("plugin"));
  CHECK_EQ(src.effects.size(), (size_t)1);
  const json proj = toProjection(d, dir);
  int seen = 0;
  for (const auto& t : proj["tracks"])
    if (t["id"] == bass) {
      ++seen;
      CHECK_EQ(t["instrument"].get<std::string>(), std::string("none"));
      CHECK(t["effects"].empty());
      CHECK(t["clips"].empty());
      CHECK(!t.contains("plugin"));
    }
  CHECK_EQ(seen, 1);
  // O motor aceita a música assim (a app recusa uma edição que o motor não toca).
  bool engineTakesIt = true;
  try { (void)core::fromJson(proj); } catch (const std::exception&) { engineTakesIt = false; }
  CHECK(engineTakesIt);
  // Guardar e abrir mantém o estado.
  const Document reopened = parseDocument(toJson(d));
  CHECK(isFrozen(*reopened.findTrack(bass)));
}

TEST("freeze: a pista congelada continua no export MIDI/Bitwig (está muda só por causa do áudio)") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("freeze-bitwig");
  freezeTrack(d, bass, renderFile(dir), dir);
  const MidiFile m = songToMidi(d);
  CHECK_EQ(m.tracks.size(), (size_t)1);
  const BitwigPlan plan = bitwigPlan(d);
  bool found = false;
  for (const auto& t : plan.tracks) if (t.trackId == bass) { found = true; CHECK_EQ(t.midiTrack, 1); }
  CHECK(found);
}

TEST("unfreeze: a pista de áudio sai, a original volta a tocar com os plugins") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("unfreeze");
  const size_t tracksBefore = d.tracks.size();
  const Printed p = freezeTrack(d, bass, renderFile(dir), dir);
  unfreezeTrack(d, bass);
  CHECK(!isFrozen(*d.findTrack(bass)));
  CHECK(!d.findTrack(bass)->mix.mute);
  CHECK(d.findTrack(p.trackId) == nullptr);
  CHECK_EQ(d.tracks.size(), tracksBefore);
  CHECK_EQ(unusedAssets(d).size(), (size_t)1);  // o ficheiro do freeze sai no próximo Save
  std::string instrument;
  const json proj = toProjection(d, dir);  // kept: a loop over a member of a temporary would read freed memory
  for (const auto& t : proj["tracks"]) if (t["id"] == bass) instrument = t["instrument"].get<std::string>();
  CHECK_EQ(instrument, std::string("plugin"));
  CHECK_THROWS(unfreezeTrack(d, bass));  // já não está congelada
}

TEST("unfreeze: se a pista de áudio já foi apagada, a original volta na mesma") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const fs::path dir = freshDir("unfreeze-apagada");
  const Printed p = freezeTrack(d, bass, renderFile(dir), dir);
  removeTrack(d, p.trackId);
  unfreezeTrack(d, bass);
  CHECK(!isFrozen(*d.findTrack(bass)));
  CHECK(!d.findTrack(bass)->mix.mute);
}

TEST("freeze: recusas claras (já congelada, áudio, sem plugins, muda, o próprio áudio do freeze)") {
  Document d = newEmptyDocument();
  const std::string bass = bassTrack(d);
  const std::string empty = addTrack(d, "", "Sem plugin");
  const std::string vox = addAudioTrack(d, "Vox");
  const std::string muted = bassTrack(d);
  trackById(d, muted).mix.mute = true;
  CHECK(!cannotFreeze(d, empty).empty());
  CHECK(!cannotFreeze(d, vox).empty());
  CHECK(!cannotFreeze(d, muted).empty());
  CHECK(!cannotFreeze(d, "nao-existe").empty());
  const fs::path dir = freshDir("freeze-recusas");
  const Printed p = freezeTrack(d, bass, renderFile(dir), dir);
  CHECK(!cannotFreeze(d, bass).empty());
  CHECK(!cannotFreeze(d, p.trackId).empty());
  const std::string before = toJson(d).dump();
  CHECK_THROWS(freezeTrack(d, bass, renderFile(dir), dir));
  CHECK_EQ(toJson(d).dump(), before);
}

CORE_TEST_MAIN
