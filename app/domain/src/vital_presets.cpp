#include "vital_presets.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include "edit.hpp"

namespace core::doc {
namespace fs = std::filesystem;

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
// JUCE's own MemoryBlock::toBase64Encoding: "<size>." then 6 bits per char, least significant bit first.
const char* kJuce64 = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
constexpr uint32_t kJuceXmlMagic = 0x21324356;  // AudioProcessor::copyXmlToBinary
const std::string kJucePrivate = "JUCEPrivateData";

std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw DocumentError(p.filename().string(), "cannot be read");
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string slug(const std::string& name) {
  std::string s;
  for (char c : lower(name)) s += (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) ? c : '-';
  return s;
}

json parsePreset(const std::string& text, const std::string& where) {
  json j = json::parse(text, nullptr, false);
  if (!j.is_object() || !j.contains("settings") || !j["settings"].is_object())
    throw DocumentError(where, "is not a Vital preset");
  return j;
}

// "... | estilo: hitech | papel: bass | tags: a, b | ..." (what saveVitalVariant writes)
std::string commentField(const std::string& comments, const std::string& key) {
  const std::string k = key + ": ";
  const auto at = comments.find(k);
  if (at == std::string::npos) return "";
  const auto end = comments.find(" | ", at);
  return comments.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}

const json& vitalPlugin(const Document& doc, const std::string& trackId) {
  for (const auto& t : doc.tracks)
    if (t.id == trackId) {
      if (!isVitalTrack(doc, trackId)) throw DocumentError("tracks." + trackId, "its instrument is not Vital");
      return t.instrument.plugin;
    }
  throw DocumentError("tracks", "no track " + trackId);
}

}  // namespace

std::string base64Encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const unsigned n = ((unsigned char)in[i] << 16) | (i + 1 < in.size() ? (unsigned char)in[i + 1] << 8 : 0) |
                       (i + 2 < in.size() ? (unsigned char)in[i + 2] : 0);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kB64[n & 63] : '=';
  }
  return out;
}

std::string base64Decode(const std::string& in) {
  std::string out;
  unsigned buf = 0;
  int bits = 0, pad = 0;
  for (char c : in) {
    if (c == '=') { ++pad; continue; }
    if (c == '\r' || c == '\n') continue;
    const char* p = std::strchr(kB64, c);
    if (!p || c == 0 || pad) throw DocumentError("state", "is not base64");
    buf = (buf << 6) | (unsigned)(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += (char)((buf >> bits) & 0xFF);
    }
  }
  return out;
}

std::string juceBase64Encode(const std::string& in) {
  const size_t chars = (in.size() * 8 + 5) / 6;
  std::string out = std::to_string(in.size()) + ".";
  for (size_t i = 0; i < chars; ++i) {
    unsigned v = 0;
    for (int b = 0; b < 6; ++b) {
      const size_t bit = i * 6 + b;
      if (bit / 8 < in.size() && ((unsigned char)in[bit / 8] >> (bit % 8) & 1)) v |= 1u << b;
    }
    out += kJuce64[v];
  }
  return out;
}

std::string juceBase64Decode(const std::string& in) {
  const auto dot = in.find('.');
  if (dot == std::string::npos || dot == 0 || dot > 10) throw DocumentError("state", "is not JUCE base64");
  for (size_t i = 0; i < dot; ++i) if (!std::isdigit((unsigned char)in[i])) throw DocumentError("state", "is not JUCE base64");
  const size_t size = std::stoul(in.substr(0, dot));
  std::string out(size, '\0');
  size_t bit = 0;
  for (size_t i = dot + 1; i < in.size(); ++i) {
    const char* p = in[i] ? std::strchr(kJuce64, in[i]) : nullptr;
    if (!p) throw DocumentError("state", "is not JUCE base64");
    const unsigned v = (unsigned)(p - kJuce64);
    for (int b = 0; b < 6; ++b, ++bit)
      if (bit / 8 < size && (v >> b & 1)) out[bit / 8] = (char)((unsigned char)out[bit / 8] | 1u << (bit % 8));
  }
  if (bit < size * 8) throw DocumentError("state", "JUCE base64 is too short");
  return out;
}

std::vector<VitalPreset> loadVitalLibrary(const fs::path& coreDir, const fs::path& userDir) {
  std::vector<VitalPreset> lib;
  const json index = json::parse(readFile(coreDir / "presets.json"), nullptr, false);
  if (!index.is_object() || index.value("format", "") != "core-ai-vital-presets" || !index.contains("presets") ||
      !index["presets"].is_array())
    throw DocumentError("presets.json", "is not Core's Vital preset index");
  for (const auto& p : index["presets"]) {
    if (!p.is_object() || !p.contains("id") || !p.contains("file")) throw DocumentError("presets.json", "a preset needs id and file");
    VitalPreset v;
    v.id = p["id"].get<std::string>();
    v.name = p.value("name", v.id);
    v.style = p.value("style", "");
    v.role = p.value("role", "");
    v.description = p.value("description", "");
    v.bpm = p.value("bpm", 0);
    v.origin = "core";
    if (p.contains("tags") && p["tags"].is_array())
      for (const auto& t : p["tags"]) if (t.is_string()) v.tags.push_back(t.get<std::string>());
    v.file = coreDir / fs::u8path(p["file"].get<std::string>());
    lib.push_back(std::move(v));
  }
  std::error_code ec;
  if (!userDir.empty() && fs::is_directory(userDir, ec)) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(userDir, ec))
      if (e.is_regular_file() && lower(e.path().extension().string()) == ".vital") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      json j;
      try { j = parsePreset(readFile(f), f.filename().string()); } catch (const DocumentError&) { continue; }  // not a preset: skipped
      VitalPreset v;
      v.name = j.value("preset_name", f.stem().string());
      if (v.name.empty()) v.name = f.stem().string();
      v.id = "user:" + slug(v.name);
      const std::string comments = j.value("comments", "");
      v.style = commentField(comments, "estilo");
      v.role = commentField(comments, "papel");
      v.description = comments.substr(0, comments.find(" | "));
      std::stringstream tags(commentField(comments, "tags"));
      for (std::string t; std::getline(tags, t, ',');) {
        t.erase(0, t.find_first_not_of(' '));
        if (!t.empty()) v.tags.push_back(t);
      }
      v.origin = "user";
      v.file = f;
      lib.push_back(std::move(v));
    }
  }
  return lib;
}

std::vector<VitalPreset> findVitalPresets(const std::vector<VitalPreset>& lib, const std::string& style, const std::string& role) {
  std::vector<VitalPreset> out;
  for (const auto& p : lib)
    if ((style.empty() || p.style == style) && (role.empty() || p.role == role)) out.push_back(p);
  std::stable_sort(out.begin(), out.end(), [](const VitalPreset& a, const VitalPreset& b) {
    if (a.origin != b.origin) return a.origin == "core";
    return a.name < b.name;
  });
  return out;
}

const VitalPreset* findVitalPreset(const std::vector<VitalPreset>& lib, const std::string& key) {
  for (const auto& p : lib) if (p.id == key) return &p;
  for (const auto& p : lib) if (lower(p.name) == lower(key)) return &p;
  return nullptr;
}

std::string vitalState(const std::string& presetJson, const VitalMacros& macros) {
  json j = parsePreset(presetJson, "preset");
  for (size_t i = 0; i < macros.size(); ++i) {
    if (!(macros[i] >= 0.0 && macros[i] <= 1.0)) throw DocumentError(kVitalMacros[i], "must be between 0 and 1");
    j["settings"]["macro_control_" + std::to_string(i + 1)] = macros[i];
  }
  // What Vital's own getStateInformation writes (MemoryOutputStream::writeString): the JSON and a NUL. Its VST3
  // component state is that (JUCE's wrapper adds private data after it, optional on load).
  std::string component = j.dump();
  component.push_back('\0');
  // What JUCE's VST3 host (the engine) keeps as a plugin's state: copyXmlToBinary of
  // <VST3PluginState><IComponent>JUCE base64</IComponent></VST3PluginState>. Without IEditController the host
  // gives the controller the component state (setComponentState), as on any load.
  const std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?> <VST3PluginState><IComponent>" +
                          juceBase64Encode(component) + "</IComponent></VST3PluginState>";
  std::string bin;
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) bin.push_back((char)(v >> (8 * i) & 0xFF)); };
  put32(kJuceXmlMagic);
  put32((uint32_t)xml.size());
  bin += xml;
  bin.push_back('\0');
  return base64Encode(bin);
}

std::string vitalComponentState(const std::string& state) {
  const std::string bin = base64Decode(state);
  auto get32 = [&](size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)(unsigned char)bin[at + i] << (8 * i);
    return v;
  };
  if (bin.size() < 9 || get32(0) != kJuceXmlMagic) throw DocumentError("state", "is not a JUCE VST3 host state");
  const std::string xml = bin.substr(8, std::min<size_t>(get32(4), bin.size() - 8));
  const auto a = xml.find("<IComponent>"), b = xml.find("</IComponent>");
  if (a == std::string::npos || b == std::string::npos || b < a) throw DocumentError("state", "has no IComponent");
  std::string text;
  for (char c : xml.substr(a + 12, b - a - 12)) if (!std::isspace((unsigned char)c)) text += c;
  return juceBase64Decode(text);
}

std::string vitalPresetFromState(const std::string& state) {
  std::string data = vitalComponentState(state);
  // JUCE's VST3 wrapper may put its private data after the plugin's: [..][int64 size]["JUCEPrivateData"]
  if (data.size() >= kJucePrivate.size() + 16 && data.compare(data.size() - kJucePrivate.size(), kJucePrivate.size(), kJucePrivate) == 0) {
    uint64_t n = 0;
    const size_t at = data.size() - kJucePrivate.size() - 8;
    for (int i = 0; i < 8; ++i) n |= (uint64_t)(unsigned char)data[at + i] << (8 * i);
    if (n + 16 <= at) data.resize(at - n - 8);
  }
  const auto nul = data.find('\0');
  if (nul != std::string::npos) data.resize(nul);
  parsePreset(data, "state");
  return data;
}

bool isVitalTrack(const Document& doc, const std::string& trackId) {
  for (const auto& t : doc.tracks) {
    if (t.id != trackId) continue;
    if (t.instrument.kind != "plugin" || !t.instrument.plugin.is_object()) return false;
    const std::string name = lower(t.instrument.plugin.value("name", ""));
    const std::string file = lower(fs::u8path(t.instrument.plugin.value("file", "")).stem().string());
    return name == "vital" || file == "vital";
  }
  return false;
}

void setVitalPreset(Document& doc, const std::string& trackId, const VitalPreset& preset, const VitalMacros& macros) {
  vitalPlugin(doc, trackId);  // throws when it is not Vital
  const std::string state = vitalState(readFile(preset.file), macros);
  json& plugin = trackById(doc, trackId).instrument.plugin;
  plugin["state"] = state;
  json m = json::object();
  for (size_t i = 0; i < macros.size(); ++i) m[lower(kVitalMacros[i])] = macros[i];
  plugin["vitalPreset"] = {{"id", preset.id}, {"name", preset.name}, {"macros", m}};
}

fs::path saveVitalVariant(const Document& doc, const std::string& trackId, const VitalPreset& base,
                          const std::string& label, const fs::path& userDir) {
  const json& plugin = vitalPlugin(doc, trackId);
  if (!plugin.contains("state") || !plugin["state"].is_string())
    throw DocumentError("tracks." + trackId, "Vital has no state yet (play it once, or put a preset on it)");
  json j = parsePreset(vitalPresetFromState(plugin["state"].get<std::string>()), "state");
  std::string clean;
  for (char c : label) if (!std::strchr("<>:\"/\\|?*", c) && (unsigned char)c >= 32) clean += c;
  if (clean.empty()) clean = "variante";
  std::string tags;
  for (const auto& t : base.tags) tags += (tags.empty() ? "" : ", ") + t;
  fs::create_directories(userDir);
  std::string name = base.name + " - " + clean;
  fs::path file = userDir / fs::u8path(name + ".vital");
  for (int n = 2; fs::exists(file); ++n) file = userDir / fs::u8path(name + " " + std::to_string(n) + ".vital");
  name = u8(file.stem());
  j["preset_name"] = name;
  j["author"] = "Diogo";
  j["comments"] = "Variante de " + base.name + " | estilo: " + base.style + " | papel: " + base.role + " | tags: " + tags;
  std::ofstream out(file, std::ios::binary);
  out << j.dump();
  if (!out) throw DocumentError(file.filename().string(), "cannot be written");
  return file;
}

}  // namespace core::doc
