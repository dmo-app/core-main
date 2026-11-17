// Testes da ronda 3 do audit (ROUND3-PR9-PR4AI.md, R3-A a R3-G): cada teste repete o caso do relatorio.
// R3-B tem uma parte no desktop (o cartao cinzento ja nao abre a tool) que so o desktop_self_test do Windows ve;
// aqui fica a parte do dominio (a frase so diz o que escreveu).
#include <fstream>
#include "support/check.hpp"
#include "edit.hpp"
#include "role_tools.hpp"
#include "roles.hpp"

using namespace core::doc;

namespace {
json fakePlugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", "VST3-" + name + "-1234-abcd"}, {"name", name}, {"vendor", "Test"}, {"file", "C:/VST3/" + name + ".vst3"}};
}
RoleBinding reverbWith(bool size) {
  json params = json::array({{{"id", "0"}, {"name", "Mix"}}});
  if (size) params.push_back({{"id", "2"}, {"name", "Decay"}});
  return guessRoleBinding("reverb", fakePlugin(size ? "ValhallaRoom" : "OneKnobVerb"), params);
}
RoleMap reverbOnly(bool size = true) {
  RoleMap m;
  m.roles["reverb"] = reverbWith(size);
  return m;
}
std::vector<std::string> noteTracks(const Document& d) {
  std::vector<std::string> out;
  for (const auto& t : d.tracks) if (playsPatterns(t)) out.push_back(t.id);
  return out;
}
std::string trackNamed(const Document& d, const std::string& part) {
  for (const auto& t : d.tracks) if (t.name.find(part) != std::string::npos) return t.id;
  return "";
}
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

TEST("ronda 3, R3-A: toolRoll numa pista cheia (64 padroes) recusa e nao corta nada") {
  Document d = newTemplateDocument();
  d.lengthTicks = 64 * 4 * kPpq;
  const std::string bass = trackNamed(d, "ass");
  CHECK(!bass.empty());
  Track& t = trackById(d, bass);
  while (t.patterns.size() < 64) addPattern(t, 4 * kPpq);
  addClip(t, t.patterns.front().id, 0, 64 * 4 * kPpq);  // um clip que passa pelo sitio do roll
  const std::string before = toJson(d).dump();
  CHECK_THROWS(toolRoll(d, bass, 14 * 4 * kPpq, 16 * 4 * kPpq, "steps"));
  CHECK_EQ(toJson(d).dump(), before);  // nem um clip cortado
}

TEST("ronda 3, R3-D: um reverb sem tamanho diz que o huge nao teve tamanho") {
  Document d = newTemplateDocument();
  const SharedBusResult no = toolSharedBus(d, noteTracks(d), reverbOnly(false), "reverb", "huge");
  CHECK(!no.sizeSet);
  CHECK(has(no.sentence, "no size control"));
  Document e = newTemplateDocument();
  const SharedBusResult yes = toolSharedBus(e, noteTracks(e), reverbOnly(true), "reverb", "huge");
  CHECK(yes.sizeSet);
  CHECK(!has(yes.sentence, "no size control"));
}

TEST("ronda 3, R3-E: uma pista com 8 sends fica como estava e e dita pelo nome; quem ja tinha um R-Core muda sem o perder") {
  Document d = newTemplateDocument();
  const auto tracks = noteTracks(d);
  CHECK(tracks.size() >= 3);
  const RoleMap m = reverbOnly();
  // A: a terceira pista cheia (8 sends a outros buses), sem R-Core: fica de fora, com nome e razao, sem mudar.
  const std::string full = tracks[2];
  while (trackById(d, full).mix.sends.size() < (size_t)kMaxSends) addSend(d, full, addBusTrack(d, "Bus"), -10, false);
  const SharedBusResult room = toolSharedBus(d, {tracks[0], full}, m, "reverb", "room");
  CHECK_EQ(room.sends, 1);
  CHECK_EQ(room.skipped.size(), (size_t)1);
  CHECK(has(room.sentence, trackById(d, full).name));
  CHECK(has(room.sentence, "maximum"));
  CHECK_EQ(trackById(d, full).mix.sends.size(), (size_t)kMaxSends);
  for (const auto& s : trackById(d, full).mix.sends) CHECK(s.to != room.busId);
  // B: a segunda pista com 7 sends + o do room (8): ao mudar para hall o send do room da lugar ao do hall.
  const std::string second = tracks[1];
  (void)toolSharedBus(d, {second}, m, "reverb", "room");
  while (trackById(d, second).mix.sends.size() < (size_t)kMaxSends) addSend(d, second, addBusTrack(d, "Bus"), -10, false);
  const SharedBusResult hall = toolSharedBus(d, {second}, m, "reverb", "hall");
  CHECK_EQ(hall.sends, 1);
  CHECK(hall.skipped.empty());
  int toHall = 0, toRoom = 0;
  for (const auto& s : trackById(d, second).mix.sends) { toHall += s.to == hall.busId; toRoom += s.to == room.busId; }
  CHECK_EQ(toHall, 1);
  CHECK_EQ(toRoom, 0);
  (void)parseDocument(toJson(d));
}

TEST("ronda 3, R3-F: kind e style aceitam maiusculas, como preset, intent e section") {
  Document d = newTemplateDocument();
  const SharedBusResult r = toolSharedBus(d, noteTracks(d), reverbOnly(), "Reverb", "Room");
  CHECK(!r.busId.empty());
  const std::string bass = trackNamed(d, "ass");
  const int64_t end = std::min<int64_t>(d.lengthTicks, 8 * 4 * kPpq), start = end - 4 * kPpq;
  CHECK(toolRoll(d, bass, start, end, "Steps").notes > 0);
  CHECK(toolRoll(d, bass, start, end, "GRADUAL").notes > 0);
  CHECK_THROWS(toolSharedBus(d, noteTracks(d), reverbOnly(), "chorus", "room"));
}

TEST("ronda 3, R3-G: seccoes que o hall nao conhece nao fazem a frase dizer 'quieter in the drops'") {
  Document d = newTemplateDocument();
  d.sections.clear();
  const int64_t bar = 4 * kPpq;
  addSection(d, "Verse 1", 0, 2 * bar);
  addSection(d, "Chorus", 2 * bar, 2 * bar);
  const SharedBusResult r = toolSharedBus(d, noteTracks(d), reverbOnly(), "reverb", "hall");
  CHECK_EQ(r.sections, 0);
  CHECK(!has(r.sentence, "quieter"));
  // Com um drop a sério a frase volta.
  Document e = newTemplateDocument();
  e.sections.clear();
  addSection(e, "Verse", 0, 2 * bar);
  addSection(e, "Drop 1", 2 * bar, 2 * bar);
  const SharedBusResult s = toolSharedBus(e, noteTracks(e), reverbOnly(), "reverb", "hall");
  CHECK_EQ(s.sections, 1);
  CHECK(has(s.sentence, "quieter in the drops"));
}

TEST("ronda 3, R3-B: o F-Core so promete o que escreveu (impact sem drive nao fala de grit)") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  const int64_t end = std::min<int64_t>(d.lengthTicks, 8 * 4 * kPpq), start = end - 4 * kPpq;
  const RoleToolResult r = toolIntentFx(d, bass, reverbOnly(), "impact", start, end, 0.7);
  CHECK(has(r.sentence, "space"));
  CHECK(!has(r.sentence, "grit"));
  CHECK_EQ(r.skipped.size(), (size_t)1);
  const RoleToolResult i = toolIntentFx(d, bass, reverbOnly(), "intro", start, end, 0.7);
  CHECK(has(i.sentence, "wet to dry"));
  CHECK(!has(i.sentence, "dark"));  // sem filtro
}

TEST("ronda 3, R3-C: os textos novos do painel Tools e dos menus tem traducao") {
  std::ifstream f(CORE_LANG_DIR "/pt-PT.json", std::ios::binary);
  const json lang = json::parse(f);
  for (const char* key :
       {"TOOLS", "Settings > Roles: choose the plugin of {}", "Tools: Core's tools as cards (the AI uses the same ones)",
        "Patterns: the channel rack and the templates (Ctrl+P)", "Patterns window (channel rack, templates)",
        "Give the silent tracks their role's plugin ({} silent)", "No plugins for the roles yet",
        "Settings > Roles says which plugin plays each part (Drums: sfizz, Bass / Lead / Pad: Vital...). Choose them there, then try again.",
        "{} tracks play their role's plugin now (Ctrl+Z puts them back).",
        "Still silent (no role guessed, or no plugin for it in Settings > Roles): {}.",
        "The role map could not be read ({}): Settings > Roles.", "Choose a track with notes first (click its name)",
        "Select the tracks that share it first", "No sections yet: right-click the timeline to add one", "The whole song",
        "A new track", "Open the Patterns window\xE2\x80\xA6", "Into the drop after", "Over the build-up", "Steps (1/4, 1/8, 1/16, 1/32)",
        "Gradual (speeding up all the way)", "Instrument: {}", "New track: {}", "F-Core: {}",
        // O separador PLUGINS e a zona por compassos/pistas (Diogo 2026-11-17).
        "PLUGINS", "instruments", "effects", "First bar:", "Last bar:", "Apply to:", "Amount (0 to 1):",
        "Tools and plugins: Core's tools as cards, and the installed plugins by maker",
        "New track with it", "Replace the instrument", "Add as effect", "Click a track with notes first",
        "Click a track first", "No plugins of this maker", "Choose bars and tracks\xE2\x80\xA6",
        "This track", "This track ({})", "Selected tracks ({})", "All tracks with notes",
        "Choose bars between 1 and {}, the first no later than the last."})
    CHECK(lang["strings"].contains(key));
}

CORE_TEST_MAIN
