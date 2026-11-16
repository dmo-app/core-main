// O template hitech com os plugins novos (Diogo, 2026-11-16: "pegar num dos antigos, deixar apenas o MIDI e aplicar o
// resto"): keepOnlyMidi tira os sons e os efeitos antigos e deixa as notas, os padroes, os clips, as seccoes e o tempo;
// o template "hitech-plugins" (feito de hitech-ref) abre, toca, guarda e reabre igual; ao criar uma musica a partir dele
// cada pista toca o plugin do seu papel (Settings > Roles: Kick 3, Serum 2, Xfer 909...) e os buses levam o reverb e o
// delay dos papeis. Sem plugins verdadeiros: o dominio so guarda referencias.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include "support/check.hpp"
#include "roles.hpp"
#include "storage.hpp"
#include "templates.hpp"
#include "core/project.hpp"
#include "core/server.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
fs::path tempDir(const std::string& name) {
  fs::path p = fs::temp_directory_path() / ("core-template-midi-tests-" + name);
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}
const SongTemplate& findTemplate(const TemplateLibrary& lib, const std::string& id) {
  const TemplateEntry* e = lib.find(id);
  if (!e) throw std::runtime_error("template '" + id + "' nao esta em templates/");
  return e->tmpl;
}
TemplateLibrary builtins() { return loadTemplates(CORE_TEMPLATES_DIR, tempDir("user-vazia"), true); }

// O MIDI de um documento, para comparar: por pista, os padroes com as notas e os clips.
json midiOf(const Document& d) {
  json tracks = json::array();
  for (const auto& t : d.tracks) {
    json j = toJson(d)["tracks"][tracks.size()];
    tracks.push_back({{"id", t.id}, {"patterns", j["patterns"]}, {"clips", j["clips"]}});
  }
  return {{"bpm", d.bpm}, {"lengthTicks", d.lengthTicks}, {"sections", toJson(d)["sections"]}, {"tracks", tracks}};
}
bool startsWithFx(const std::string& p) { return p.rfind("fx:", 0) == 0; }
bool soundAutomation(const Automation& a) { return a.param != "gainDb" && a.param != "pan"; }

json fakePlugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", "VST3-" + name + "-1234-abcd"}, {"name", name}, {"vendor", "Test"}, {"file", "C:/VST3/" + name + ".vst3"}};
}
// O mapa de papeis do Diogo (lista-manual.md): Kick 3, Serum 2, Xfer 909, Pro-R 2, Timeless 3.
RoleMap diogoMap() {
  RoleMap m;
  m.roles["kick"].plugin = fakePlugin("Kick 3");
  m.roles["bass"].plugin = fakePlugin("Serum 2 Bass");
  m.roles["lead"].plugin = fakePlugin("Serum 2 Lead");
  m.roles["pluck"].plugin = fakePlugin("Serum 2 Pluck");
  m.roles["pad"].plugin = fakePlugin("Serum 2 Pad");
  m.roles["drums"].plugin = fakePlugin("Xfer 909");
  m.roles["reverb"] = guessRoleBinding("reverb", fakePlugin("Pro-R 2"), json::array({{{"id", "0"}, {"name", "Mix"}}, {{"id", "1"}, {"name", "Decay Rate"}}}));
  m.roles["delay"] = guessRoleBinding("delay", fakePlugin("Timeless 3"), json::array({{{"id", "3"}, {"name", "Mix"}}, {{"id", "4"}, {"name", "Feedback"}}}));
  return m;
}
}  // namespace

TEST("keepOnlyMidi: hitech-ref fica so com o MIDI (notas, padroes, clips, seccoes, tempo)") {
  const TemplateLibrary lib = builtins();
  const Document old = findTemplate(lib, "hitech-ref").document;
  const Document d = keepOnlyMidi(old);
  CHECK(midiOf(d) == midiOf(old));
  CHECK_EQ(d.tracks.size(), old.tracks.size());
  CHECK(d.masterEffects.empty());
  for (const auto& a : d.masterAutomation) CHECK(!startsWithFx(a.param));
  for (const auto& t : d.tracks) {
    CHECK(t.effects.empty());
    CHECK(t.modulators.empty());
    for (const auto& a : t.automation) CHECK(!soundAutomation(a));
    if (t.instrument.kind == "bus") continue;
    CHECK_EQ(t.instrument.kind, std::string("none"));  // os sons antigos (W-Core, kick, hats) sairam
    CHECK(t.instrument.params.empty());
    CHECK(t.name.find("W-Core") == std::string::npos);
  }
  // A parte de cada pista fica no nome do papel: "Accents" e "Impacts" tocavam o kick antigo.
  CHECK_EQ(guessInstrumentRole(*d.findTrack("accents")), std::string("kick"));
  CHECK_EQ(guessInstrumentRole(*d.findTrack("impact")), std::string("kick"));
  CHECK_EQ(guessInstrumentRole(*d.findTrack("fm")), std::string("lead"));
  // As sends para os buses ficam (o reverb e o delay entram nos buses pelos papeis).
  int sends = 0;
  for (const auto& t : d.tracks) sends += (int)t.mix.sends.size();
  CHECK(sends > 0);
}

TEST("hitech-plugins: o template vai com a app, listado, com o MIDI do hitech-ref e os plugins da lista") {
  const TemplateLibrary lib = builtins();
  const SongTemplate& t = findTemplate(lib, "hitech-plugins");
  CHECK(!isBaseTemplate(t));
  CHECK(usesRolePlugins(t));
  CHECK_EQ(t.document.bpm, 190.0);
  // O MIDI do hitech-ref; so os pratos e a perc mudam de nota para o mapa GM do kit do sfizz (FreePats GM.sfz).
  Document want = keepOnlyMidi(findTemplate(lib, "hitech-ref").document);
  const std::map<std::string, std::map<int, int>> gm = {{"crash", {{46, 49}}}, {"perc", {{60, 75}, {61, 69}}}};
  for (auto& tr : want.tracks)
    if (auto g = gm.find(tr.id); g != gm.end())
      for (auto& p : tr.patterns) for (auto& n : p.notes) if (auto k = g->second.find(n.pitch); k != g->second.end()) n.pitch = k->second;
  CHECK(midiOf(t.document) == midiOf(want));
  // Os plugins do scan do PC do Diogo (core-engine scan, 16/11), pela lista dele.
  const std::map<std::string, std::string> plays = {{"kick", "sfizz"},   {"accents", "sfizz"},  {"impact", "sfizz"},
                                                    {"bass", "Serum 2"}, {"lead", "Serum 2"},   {"fm", "Serum 2"},
                                                    {"arp", "Serum 2"},  {"hats", "sfizz"},     {"open-hats", "sfizz"},
                                                    {"perc", "sfizz"},   {"crash", "sfizz"}};
  for (const auto& tr : t.document.tracks) {
    auto it = plays.find(tr.id);
    if (it != plays.end()) {
      CHECK_EQ(tr.instrument.kind, std::string("plugin"));
      CHECK_EQ(tr.instrument.plugin.value("name", std::string()), it->second);
      CHECK(tr.effects.empty());
      if (it->second == "sfizz") {  // com o kit: o estado VST3 que o JUCE le (XML VST3PluginState, magic "VC2!")
        const std::string st = tr.instrument.plugin.value("state", std::string());
        CHECK(st.rfind("VkMyI", 0) == 0);
      }
    } else {
      CHECK_EQ(tr.instrument.kind, std::string("bus"));
    }
  }
  // Buses de envio a 100% wet: Pro-R 2 (Mix = 9) no Space e no Huge, Timeless 3 (Mix = 160) no Echo.
  const std::map<std::string, std::pair<std::string, std::string>> buses = {
      {"space", {"Pro-R 2", "vst:9"}}, {"huge", {"Pro-R 2", "vst:9"}}, {"echo", {"Timeless 3", "vst:160"}}};
  for (const auto& [id, want] : buses) {
    const Track* b = t.document.findTrack(id);
    CHECK(b != nullptr);
    if (!b) continue;
    CHECK_EQ(b->effects.size(), (size_t)1);
    CHECK_EQ(b->effects[0]["plugin"].value("name", std::string()), want.first);
    CHECK_EQ(b->automation.size(), (size_t)1);
    if (b->automation.size() == 1) {
      CHECK_EQ(b->automation[0].param, "fx:" + b->effects[0].value("id", std::string()) + ":" + want.second);
      CHECK_EQ(b->automation[0].points.at(0).value, 1.0);
    }
  }
  CHECK_EQ(t.document.masterEffects.size(), (size_t)2);
  CHECK_EQ(t.document.masterEffects[0]["plugin"].value("name", std::string()), std::string("Pro-Q 4"));
  CHECK_EQ(t.document.masterEffects[1]["plugin"].value("name", std::string()), std::string("Pro-L 2"));
  // Listado na app (sem withBases).
  const TemplateLibrary listed = loadTemplates(CORE_TEMPLATES_DIR, tempDir("user-vazia-2"));
  CHECK(listed.find("hitech-plugins") != nullptr);
}

TEST("hitech-plugins: abre e o motor toca-o (sem NaN, as notas chegam ao motor)") {
  const Document d = documentFromTemplate(findTemplate(builtins(), "hitech-plugins"));
  core::Project p = core::fromJson(toProjection(d));
  p.lengthTicks = std::min<int64_t>(p.lengthTicks, 8 * 4 * core::kPpq);
  core::RenderResult r = core::renderProject(p, 48000, 512, false, 0.1, nullptr, nullptr, nullptr, 0);
  bool finite = true;
  for (float s : r.L) finite = finite && std::isfinite(s);
  CHECK(finite);
  CHECK(!r.L.empty());
}

TEST("hitech-plugins: os papeis do PC nao trocam os plugins do template nem duplicam os efeitos dos buses") {
  Document d = documentFromTemplate(findTemplate(builtins(), "hitech-plugins"));
  const json before = toJson(d);
  CHECK_EQ(giveSongRolePlugins(d, RoleMap{}, nullptr), 0);
  std::vector<std::string> left;
  CHECK_EQ(giveSongRolePlugins(d, diogoMap(), &left), 0);
  CHECK(left.empty());
  CHECK(toJson(d) == before);
}

// giveSongRolePlugins numa musica so com MIDI: cada pista pelo seu papel e os buses com reverb e delay.
TEST("giveSongRolePlugins: keepOnlyMidi(hitech-ref) toca pelos papeis") {
  Document d = keepOnlyMidi(findTemplate(builtins(), "hitech-ref").document);
  std::vector<std::string> left;
  CHECK(giveSongRolePlugins(d, diogoMap(), &left) > 0);
  CHECK(left.empty());
  const std::map<std::string, std::string> want = {{"kick", "Kick 3"}, {"accents", "Kick 3"}, {"bass", "Serum 2 Bass"},
                                                   {"hats", "Xfer 909"}, {"crash", "Xfer 909"}, {"fm", "Serum 2 Lead"},
                                                   {"arp", "Serum 2 Pluck"}};
  for (const auto& [id, name] : want) CHECK_EQ(d.findTrack(id)->instrument.plugin.value("name", std::string()), name);
  CHECK(findRoleEffect(d, "space", "reverb") >= 0);
  CHECK(findRoleEffect(d, "huge", "reverb") >= 0);
  CHECK(findRoleEffect(d, "echo", "delay") >= 0);
  CHECK_EQ(giveSongRolePlugins(d, diogoMap(), nullptr), 0);
}

TEST("hitech-plugins: guardar e reabrir da o mesmo (template e musica)") {
  const TemplateLibrary lib = builtins();
  SongTemplate t = findTemplate(lib, "hitech-plugins");
  const fs::path user = tempDir("user-guardar");
  saveTemplate(t, loadTemplates(CORE_TEMPLATES_DIR, user, true), user);
  const TemplateLibrary again = loadTemplates(CORE_TEMPLATES_DIR, user, true);
  const TemplateEntry* e = again.find("hitech-plugins");
  CHECK(e != nullptr);
  if (e) {
    CHECK(!e->builtin);
    CHECK(toJson(e->tmpl.document) == toJson(t.document));
  }
  // A musica feita do template, com os plugins dos papeis, guardada e aberta.
  Document song = documentFromTemplate(t);
  giveSongRolePlugins(song, diogoMap(), nullptr);
  const fs::path file = tempDir("musica") / "hitech.core";
  saveFile(song, file);
  const OpenResult opened = openFile(file);
  CHECK(toJson(opened.document) == toJson(song));
}

// Bug visto ao gerar o template (16/11): o Timeless 3 tem "Feedback Cross Mix" (id 87) antes de "Mix" (id 160), e o
// macro Mix do papel Delay apanhava o primeiro. Um nome igual a palavra ganha a um que so a contem.
TEST("papel Delay no Timeless 3: o macro Mix e o parametro Mix, nao Feedback Cross Mix") {
  const json params = json::array({{{"id", "85"}, {"name", "Feedback"}}, {{"id", "87"}, {"name", "Feedback Cross Mix"}},
                                   {{"id", "160"}, {"name", "Mix"}}, {{"id", "161"}, {"name", "Lock Mix"}}});
  const RoleBinding b = guessRoleBinding("delay", fakePlugin("Timeless 3"), params);
  CHECK_EQ(b.params.at("mix"), std::string("160"));
  CHECK_EQ(b.params.at("feedback"), std::string("85"));
}

CORE_TEST_MAIN
