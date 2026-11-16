// Gera templates/hitech-plugins.core-template.json: o MIDI do hitech-ref (keepOnlyMidi) com os plugins da lista do
// Diogo, identificados pelo scan do Core no PC dele (core-engine scan / params, 16/11).
#include "templates.hpp"
#include "roles.hpp"
#include "edit.hpp"
#include <cstdio>
#include <fstream>
#include <map>
using namespace core::doc;
int main(int, char** argv) {
  json scan; std::ifstream(argv[2]) >> scan;
  auto plug = [&](const char* key, const char* name) {
    for (auto& p : scan["scan"][key]) if (p["name"] == name) return p;
    throw std::runtime_error(std::string("sem ") + name);
  };
  auto params = [&](const char* key) { return scan["params"][key][0]["params"]; };
  RoleMap m;
  m.roles["kick"].plugin = plug("Kick 3", "Kick 3");
  for (const char* r : {"bass", "lead", "pluck", "pad"}) m.roles[r].plugin = plug("Serum2", "Serum 2");
  m.roles["drums"].plugin = plug("sfizz", "sfizz");  // Diogo 16/11: "usar sfizz como bateria", kit FreePats GM
  m.roles["reverb"] = guessRoleBinding("reverb", plug("FabFilter Pro-R 2", "Pro-R 2"), params("FabFilter Pro-R 2"));
  m.roles["delay"] = guessRoleBinding("delay", plug("FabFilter Timeless 3", "Timeless 3"), params("FabFilter Timeless 3"));
  for (auto& [r, b] : m.roles) for (auto& [mac, id] : b.params) std::fprintf(stderr, "%s.%s -> %s\n", r.c_str(), mac.c_str(), id.c_str());

  TemplateLibrary lib = loadTemplates(argv[1], "/nonexistent", true);
  Document d = keepOnlyMidi(lib.find("hitech-ref")->tmpl.document);
  // As notas dos pratos e da perc no mapa GM do kit (FreePats GM.sfz: 42 hat, 46 open, 49 crash, 69 cabasa, 75 claves).
  const std::map<std::string, std::map<int, int>> gm = {{"crash", {{46, 49}}}, {"perc", {{60, 75}, {61, 69}}}};
  for (auto& t : d.tracks) {
    auto it = gm.find(t.id);
    if (it == gm.end()) continue;
    for (auto& p : t.patterns) for (auto& n : p.notes) { auto k = it->second.find(n.pitch); if (k != it->second.end()) n.pitch = k->second; }
  }
  std::vector<std::string> left;
  giveSongRolePlugins(d, m, &left);
  for (auto& n : left) std::fprintf(stderr, "sem plugin: %s\n", n.c_str());
  // Os buses sao de envio: o reverb e o delay a 100% wet (um ponto de automacao no macro Mix do papel).
  for (auto& t : d.tracks) {
    const std::string role = guessBusRole(t);
    if (role.empty()) continue;
    const std::string fx = ensureRoleEffect(d, t.id, role, m);
    Automation a; a.param = roleParamKey(m, role, fx, "mix"); a.points.push_back({0, 1.0});
    t.automation.push_back(a);
  }
  // Master: Pro-Q 4 e Pro-L 2 (a lista do Diogo: EQ e limiter).
  addPluginEffect(d, "", plug("FabFilter Pro-Q 4", "Pro-Q 4"));
  addPluginEffect(d, "", plug("FabFilter Pro-L 2", "Pro-L 2"));

  TemplateInfo info;
  info.id = "hitech-plugins";
  info.name = "Hi-tech 190 (plugins)";
  info.style = "Hi-tech";
  info.description = "The MIDI of 'Hi-tech 190 (from Wake Up and Remember)' with its old sounds and effects reset: "
    "kick, KBBB bass in A, hats, percussion, crashes, lead, FM answer and arp over 296 bars at 190 BPM (intro 8, breaks of 8 and 16). "
    "Kick, Accents, Impacts: Kick 3. Bass, Lead, FM answer, Arp: Serum 2. Space and Huge buses: Pro-R 2; Echo bus: Timeless 3 (100% wet). "
    "Master: Pro-Q 4, Pro-L 2. Hats, Percussion and Crash: sfizz with the FreePats GM kit (crash on 49, percussion on claves 75 and cabasa 69). Choose the presets, then save it as your own template.";
  info.source = {{"kind", "builtin"}, {"from", "hitech-ref"}, {"roles", true}};
  SongTemplate t = templateFromDocument(d, info);
  t.document.name = "Hi-tech 190 (plugins)";
  TemplateLibrary empty;
  std::printf("%s\n", saveTemplate(t, empty, argv[3]).string().c_str());
}
