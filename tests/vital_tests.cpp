// Testes dos presets Vital da AI (vital_presets.hpp) sobre a biblioteca verdadeira em presets/vital:
// indice, procura por estilo e papel, estado VST3 (JSON terminado em NUL, base64) com os 4 macros, preset numa
// pista Vital, variantes do utilizador.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "support/check.hpp"
#include "edit.hpp"
#include "vital_presets.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
const fs::path kLib = CORE_PRESETS_DIR;
json vitalPlugin() {
  return {{"format", "vst3"}, {"id", "VST3-Vital-1234"}, {"name", "Vital"}, {"vendor", "Vital Audio"}, {"file", "C:/VST3/Vital.vst3"}};
}
fs::path tempDir(const std::string& name) {
  fs::path d = fs::temp_directory_path() / ("core-vital-tests-" + name);
  fs::remove_all(d);
  return d;
}
}  // namespace

TEST("indice: 14 presets do Core, 5 dark psycore, 5 hitech, 4 slambient, todos os ficheiros existem") {
  const auto lib = loadVitalLibrary(kLib, "");
  CHECK_EQ(lib.size(), (size_t)14);
  CHECK_EQ(findVitalPresets(lib, "darkpsycore", "").size(), (size_t)5);
  CHECK_EQ(findVitalPresets(lib, "hitech", "").size(), (size_t)5);
  CHECK_EQ(findVitalPresets(lib, "slambient", "").size(), (size_t)4);
  for (const auto& p : lib) {
    CHECK(fs::exists(p.file));
    CHECK_EQ(p.origin, std::string("core"));
    CHECK(!p.style.empty() && !p.role.empty() && !p.tags.empty());
  }
  CHECK_EQ(findVitalPresets(lib, "slambient", "bass").size(), (size_t)1);
  CHECK(findVitalPreset(lib, "dpc-kbbb-plonk") != nullptr);
  CHECK(findVitalPreset(lib, "ht snap bass") != nullptr);
  CHECK(findVitalPreset(lib, "nao existe") == nullptr);
  CHECK_THROWS(loadVitalLibrary(tempDir("vazio"), ""));
}

TEST("estado: JSON do preset terminado em NUL, em base64, com os 4 macros; e volta igual") {
  const auto lib = loadVitalLibrary(kLib, "");
  const auto* p = findVitalPreset(lib, "HT Laser Zap");
  CHECK(p != nullptr);
  std::ifstream in(p->file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), {});
  const std::string state = vitalState(text, {0.0, 0.25, 0.75, 1.0});
  // formato do host VST3 do JUCE: "VC2!" + tamanho + XML <VST3PluginState><IComponent>...</IComponent> + NUL
  const std::string bin = base64Decode(state);
  CHECK_EQ(bin.substr(0, 4), std::string("VC2!"));
  const uint32_t len = (unsigned char)bin[4] | (unsigned char)bin[5] << 8 | (unsigned char)bin[6] << 16 | (uint32_t)(unsigned char)bin[7] << 24;
  CHECK_EQ((size_t)len, bin.size() - 9);
  CHECK(bin.back() == '\0');
  CHECK(bin.find("<VST3PluginState><IComponent>") != std::string::npos);
  // o estado do componente e o JSON do Vital terminado em NUL (o que o Vital escreve com writeString)
  const std::string raw = vitalComponentState(state);
  CHECK(!raw.empty() && raw.back() == '\0');
  CHECK_EQ(raw.find('\0'), raw.size() - 1);
  const json j = json::parse(vitalPresetFromState(state));
  CHECK_NEAR(j["settings"]["macro_control_1"].get<double>(), 0.0, 1e-12);
  CHECK_NEAR(j["settings"]["macro_control_2"].get<double>(), 0.25, 1e-12);
  CHECK_NEAR(j["settings"]["macro_control_4"].get<double>(), 1.0, 1e-12);
  CHECK_EQ(j["macro1"].get<std::string>(), std::string("TOM"));
  CHECK_EQ(j["macro4"].get<std::string>(), std::string("COMPRIMENTO"));
  CHECK_EQ(j["preset_name"].get<std::string>(), std::string("HT Laser Zap"));
  CHECK_THROWS(vitalState(text, {0.5, 0.5, 1.5, 0.5}));
  CHECK_THROWS(vitalState("{\"nao\": \"vital\"}"));
  CHECK_THROWS(vitalPresetFromState("@@@"));
  CHECK_EQ(base64Decode(base64Encode(std::string("a\0b\xff", 4))), std::string("a\0b\xff", 4));
  CHECK_EQ(base64Encode("Ma"), std::string("TWE="));
  // base64 do JUCE (MemoryBlock::toBase64Encoding): tamanho, ponto, 6 bits por letra a partir do bit mais baixo
  CHECK_EQ(juceBase64Encode(std::string("\x01", 1)), std::string("1.A."));
  CHECK_EQ(juceBase64Encode(""), std::string("0."));
  CHECK_EQ(juceBase64Decode(juceBase64Encode(std::string("a\0b\xff{}", 6))), std::string("a\0b\xff{}", 6));
  CHECK_THROWS(juceBase64Decode("abc"));
  CHECK_THROWS(juceBase64Decode("9.AB"));
}

TEST("estado guardado pelo proprio Vital: os dados privados do JUCE no fim sao ignorados") {
  const std::string preset = "{\"preset_name\":\"X\",\"settings\":{\"macro_control_1\":0.5}}";
  std::string comp = preset + std::string(1, '\0');
  std::string priv = std::string(8, '\0') + "<private/>";
  comp += priv;
  const uint64_t n = priv.size() - 8;
  for (int i = 0; i < 8; ++i) comp.push_back((char)(n >> (8 * i) & 0xFF));
  comp += "JUCEPrivateData";
  const std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?> <VST3PluginState><IComponent>" + juceBase64Encode(comp) +
                          "</IComponent><IEditController>0.</IEditController></VST3PluginState>";
  std::string bin = "VC2!";
  for (int i = 0; i < 4; ++i) bin.push_back((char)(xml.size() >> (8 * i) & 0xFF));
  bin += xml + std::string(1, '\0');
  CHECK_EQ(vitalPresetFromState(base64Encode(bin)), preset);
}

TEST("pista: o preset vai para o Vital da pista; uma pista sem Vital recusa") {
  const auto lib = loadVitalLibrary(kLib, "");
  Document d;
  const std::string vital = addPluginTrack(d, vitalPlugin());
  json other = vitalPlugin();
  other["name"] = "Serum 2";
  other["file"] = "C:/VST3/Serum2.vst3";
  const std::string serum = addPluginTrack(d, other);
  CHECK(isVitalTrack(d, vital));
  CHECK(!isVitalTrack(d, serum));
  const auto* p = findVitalPreset(lib, "dpc-kbbb-plonk");
  setVitalPreset(d, vital, *p, {0.5, 0.8, 0.5, 0.3});
  const json& plugin = trackById(d, vital).instrument.plugin;
  CHECK(plugin.contains("state"));
  CHECK_EQ(plugin["vitalPreset"]["id"].get<std::string>(), std::string("dpc-kbbb-plonk"));
  CHECK_NEAR(plugin["vitalPreset"]["macros"]["sujo"].get<double>(), 0.8, 1e-12);
  const json j = json::parse(vitalPresetFromState(plugin["state"].get<std::string>()));
  CHECK_NEAR(j["settings"]["macro_control_2"].get<double>(), 0.8, 1e-12);
  CHECK_THROWS(setVitalPreset(d, serum, *p));
  CHECK_THROWS(setVitalPreset(d, "nao-existe", *p));
}

TEST("variantes: guardar o som da pista como variante do Diogo, sem apagar, e aparece na biblioteca") {
  const fs::path user = tempDir("user");
  auto lib = loadVitalLibrary(kLib, user);
  CHECK_EQ(lib.size(), (size_t)14);  // pasta do utilizador ainda nao existe: nao e erro
  Document d;
  const std::string t = addPluginTrack(d, vitalPlugin());
  const VitalPreset base = *findVitalPreset(lib, "SLA Deep Sub");
  CHECK_THROWS(saveVitalVariant(d, t, base, "Diogo 1", user));  // ainda sem estado
  setVitalPreset(d, t, base, {0.6, 0.4, 0.5, 0.5});
  const fs::path f1 = saveVitalVariant(d, t, base, "Diogo 1", user);
  const fs::path f2 = saveVitalVariant(d, t, base, "Diogo 1", user);
  CHECK(fs::exists(f1) && fs::exists(f2) && f1 != f2);
  CHECK(f1.filename() == fs::u8path("SLA Deep Sub - Diogo 1.vital"));
  lib = loadVitalLibrary(kLib, user);
  CHECK_EQ(lib.size(), (size_t)16);
  const auto subs = findVitalPresets(lib, "slambient", "bass");
  CHECK_EQ(subs.size(), (size_t)3);
  CHECK_EQ(subs[0].origin, std::string("core"));  // os do Core primeiro
  CHECK_EQ(subs[1].origin, std::string("user"));
  CHECK(std::find(subs[1].tags.begin(), subs[1].tags.end(), "sub") != subs[1].tags.end());
  const auto* v = findVitalPreset(lib, "SLA Deep Sub - Diogo 1");
  CHECK(v != nullptr);
  // a variante volta a uma pista com os macros como ficaram
  Document d2;
  const std::string t2 = addPluginTrack(d2, vitalPlugin());
  setVitalPreset(d2, t2, *v, kVitalMacrosRest);
  CHECK(isVitalTrack(d2, t2));
  std::ofstream(user / "lixo.vital") << "isto nao e json";
  CHECK_EQ(loadVitalLibrary(kLib, user).size(), (size_t)16);  // ficheiro estragado: ignorado
  fs::remove_all(user);
}

CORE_TEST_MAIN
