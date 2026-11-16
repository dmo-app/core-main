#include "synth_roles.hpp"
#include <algorithm>
#include <cctype>
#include "synth.hpp"

namespace core::ai {
namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}
}  // namespace

nlohmann::json findPluginNamed(const nlohmann::json& scanned, const std::string& name) {
  if (!scanned.is_array()) return nullptr;
  for (const auto& p : scanned) {
    if (!p.is_object() || !p.contains("id") || !p.contains("file")) continue;
    if (lower(p.value("format", "")) != "vst3") continue;
    if (lower(p.value("name", "")) == lower(name)) return p;
  }
  return nullptr;
}

nlohmann::json findSynthPlugin(const nlohmann::json& scanned) { return findPluginNamed(scanned, synth().name); }

nlohmann::json withSynthRoles(nlohmann::json roles, const nlohmann::json& scanned) {
  if (!roles.is_object()) roles = {{"version", 1}};
  if (!roles.contains("roles") || !roles["roles"].is_object()) roles["roles"] = nlohmann::json::object();
  const nlohmann::json plugin = findSynthPlugin(scanned);
  if (plugin.is_null()) return roles;
  for (const auto& role : synth().roles) {
    nlohmann::json& r = roles["roles"][role];
    if (r.is_object() && r.contains("plugin") && !r["plugin"].is_null()) continue;  // the user's choice stays
    r = {{"plugin", plugin}, {"params", nlohmann::json::object()}};
  }
  return roles;
}

nlohmann::json findPluginWith(const nlohmann::json& scanned, const std::string& word) {
  if (!scanned.is_array()) return nullptr;
  for (const auto& p : scanned) {
    if (!p.is_object() || !p.contains("id") || !p.contains("file")) continue;
    if (lower(p.value("format", "")) != "vst3") continue;
    if (lower(p.value("name", "")).find(lower(word)) != std::string::npos) return p;
  }
  return nullptr;
}

nlohmann::json withAiRoles(nlohmann::json roles, const nlohmann::json& scanned) {
  roles = withSynthRoles(std::move(roles), scanned);
  for (const auto& r : aiRoles()) {
    const nlohmann::json plugin = findPluginWith(scanned, r.match);
    if (plugin.is_null()) continue;
    nlohmann::json& b = roles["roles"][r.role];
    if (b.is_object() && b.contains("plugin") && !b["plugin"].is_null()) continue;  // the user's choice stays
    b = {{"plugin", plugin}, {"params", nlohmann::json::object()}};
    if (*r.effect) b["effect"] = r.effect;
  }
  return roles;
}

}  // namespace core::ai
