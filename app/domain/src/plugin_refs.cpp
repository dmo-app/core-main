#include "plugin_refs.hpp"
#include <algorithm>

namespace core::doc {
namespace {

std::string lowerAscii(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

std::string text(const json& j, const char* key) {
  return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

// One reference: true when it was pointed at a scanned plugin.
bool relink(json& plugin, const std::vector<json>& scanned) {
  if (!plugin.is_object()) return false;
  const std::string id = text(plugin, "id"), file = text(plugin, "file");
  if (id.empty()) return false;
  for (const auto& s : scanned)
    if (text(s, "id") == id && text(s, "file") == file) return false;  // already this computer's plugin
  const PluginKey key = pluginKey(plugin);
  if (key.uid.empty()) return false;
  const json* match = nullptr;
  for (const auto& s : scanned) {
    if (!(pluginKey(s) == key)) continue;
    if (match) return false;  // two copies of the plugin: not guessed (the user picks one)
    match = &s;
  }
  if (!match) return false;
  plugin["id"] = text(*match, "id");
  plugin["file"] = text(*match, "file");
  return true;
}

int relinkChain(json& effects, const std::vector<json>& scanned) {
  int n = 0;
  if (!effects.is_array()) return 0;
  for (auto& fx : effects)
    if (fx.is_object() && text(fx, "type") == "plugin" && fx.contains("plugin")) n += relink(fx["plugin"], scanned);
  return n;
}

}  // namespace

PluginKey pluginKey(const json& plugin) {
  // JUCE: format + "-" + name + "-" + hex(hash of the file path) + "-" + hex(uid). The name may hold '-'.
  const std::string id = text(plugin, "id");
  const size_t last = id.rfind('-');
  if (last == std::string::npos || last == 0) return {};
  const size_t hash = id.rfind('-', last - 1);
  const size_t first = id.find('-');
  if (hash == std::string::npos || first == std::string::npos || first >= hash) return {};
  PluginKey k;
  k.format = lowerAscii(id.substr(0, first));
  k.name = lowerAscii(id.substr(first + 1, hash - first - 1));
  k.uid = lowerAscii(id.substr(last + 1));
  if (k.name.empty() || k.uid.empty()) return {};
  return k;
}

int relinkPlugins(Document& d, const std::vector<json>& scanned) {
  if (scanned.empty()) return 0;
  int n = 0;
  for (auto& t : d.tracks) {
    if (t.instrument.kind == "plugin") n += relink(t.instrument.plugin, scanned);
    n += relinkChain(t.effects, scanned);
  }
  n += relinkChain(d.masterEffects, scanned);
  return n;
}

std::vector<PluginVendorGroup> pluginsByVendor(const json& plugins) {
  std::vector<PluginVendorGroup> out;
  if (!plugins.is_array()) return out;
  for (const auto& p : plugins) {
    if (!p.is_object()) continue;
    std::string vendor = text(p, "vendor");
    if (vendor.empty()) vendor = "Other";  // sem fabricante: aparece em "Other", nunca se esconde
    auto it = std::find_if(out.begin(), out.end(), [&](const PluginVendorGroup& g) { return lowerAscii(g.vendor) == lowerAscii(vendor); });
    if (it == out.end()) { out.push_back({vendor, {}}); it = out.end() - 1; }
    it->plugins.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const PluginVendorGroup& a, const PluginVendorGroup& b) {
    return lowerAscii(a.vendor) < lowerAscii(b.vendor);
  });
  for (auto& g : out)
    std::sort(g.plugins.begin(), g.plugins.end(),
              [](const json& a, const json& b) { return lowerAscii(text(a, "name")) < lowerAscii(text(b, "name")); });
  return out;
}

}  // namespace core::doc
