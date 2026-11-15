#include "plugin_help.hpp"
#include "fx_racks.hpp"  // styleLabel
#include <algorithm>
#include <fstream>

namespace core::doc {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& message) { throw DocumentError(where, message); }

std::string text(const json& v, const std::string& key, const std::string& where, bool required) {
  if (!v.contains(key) || v[key].is_null()) {
    if (required) fail(where + key, "missing");
    return "";
  }
  if (!v[key].is_string()) fail(where + key, "must be text");
  return v[key].get<std::string>();
}

const json& array(const json& v, const std::string& key) {
  static const json empty = json::array();
  if (!v.contains(key) || v[key].is_null()) return empty;
  if (!v[key].is_array()) fail(key, "must be a list");
  return v[key];
}
}  // namespace

PluginHelp pluginHelpFromJson(const json& value) {
  if (!value.is_object()) fail("", "a plugin help is an object");
  if (value.value("format", std::string()) != kPluginHelpFormat) fail("format", std::string("must be \"") + kPluginHelpFormat + "\"");
  if (!value.contains("version") || !value["version"].is_number_integer()) fail("version", "missing");
  if (value["version"].get<int>() > kPluginHelpVersion) fail("version", "made by a newer Core");
  PluginHelp h;
  h.id = text(value, "id", "", true);
  h.title = text(value, "title", "", true);
  h.summary = text(value, "summary", "", true);
  const json& steps = array(value, "steps");
  for (size_t i = 0; i < steps.size(); ++i) {
    if (!steps[i].is_string()) fail("steps[" + std::to_string(i) + "]", "must be text");
    h.steps.push_back(steps[i].get<std::string>());
  }
  const json& examples = array(value, "examples");
  for (size_t i = 0; i < examples.size(); ++i) {
    const std::string where = "examples[" + std::to_string(i) + "].";
    if (!examples[i].is_object()) fail(where, "must be an object");
    h.examples.push_back({text(examples[i], "title", where, true), text(examples[i], "text", where, true),
                          text(examples[i], "preset", where, false)});
  }
  const json& styles = array(value, "styles");
  for (size_t i = 0; i < styles.size(); ++i) {
    const std::string where = "styles[" + std::to_string(i) + "].";
    if (styles[i].is_string()) { h.styles.push_back({styles[i].get<std::string>(), ""}); continue; }
    if (!styles[i].is_object()) fail(where, "must be an object");
    h.styles.push_back({text(styles[i], "id", where, true), text(styles[i], "why", where, false)});
  }
  const json& roles = array(value, "sampleRoles");
  for (size_t i = 0; i < roles.size(); ++i) {
    const std::string where = "sampleRoles[" + std::to_string(i) + "]";
    if (!roles[i].is_string()) fail(where, "must be text");
    const std::string given = roles[i].get<std::string>();
    const auto& tracks = samplerRoles();
    // A track role ("accent": claps and snares) or a catalogue role ("fx", "vox", "hihat" -> "hat").
    const std::string role = std::find(tracks.begin(), tracks.end(), given) != tracks.end() ? given : canonicalSampleRole(given);
    if (role.empty()) fail(where, "\"" + given + "\" is not a sample role");
    h.sampleRoles.push_back(role);
  }
  return h;
}

json pluginHelpToJson(const PluginHelp& h) {
  json examples = json::array(), styles = json::array();
  for (const auto& e : h.examples) {
    json x = {{"title", e.title}, {"text", e.text}};
    if (!e.preset.empty()) x["preset"] = e.preset;
    examples.push_back(std::move(x));
  }
  for (const auto& s : h.styles) styles.push_back({{"id", s.id}, {"why", s.why}});
  return {{"format", kPluginHelpFormat}, {"version", kPluginHelpVersion}, {"id", h.id}, {"title", h.title},
          {"summary", h.summary}, {"steps", h.steps}, {"examples", examples}, {"styles", styles},
          {"sampleRoles", h.sampleRoles}};
}

PluginHelp loadPluginHelp(const std::filesystem::path& folder, const std::string& id) {
  const auto file = folder / (id + ".json");
  std::ifstream in(file, std::ios::binary);
  if (!in) fail(id, "no help file " + file.filename().string());
  json value;
  try {
    value = json::parse(in);
  } catch (const std::exception& e) {
    fail(id, std::string("not JSON: ") + e.what());
  }
  PluginHelp h = pluginHelpFromJson(value);
  if (h.id != id) fail("id", "is \"" + h.id + "\", the file is " + file.filename().string());
  return h;
}

std::vector<HelpSamples> helpSamples(const SampleIndex& index, const PluginHelp& help, const std::string& style, int perRole) {
  std::vector<HelpSamples> out;
  const auto& tracks = samplerRoles();
  for (const auto& role : help.sampleRoles) {
    std::vector<SampleSuggestion> found;
    if (std::find(tracks.begin(), tracks.end(), role) != tracks.end()) {
      found = suggestSamples(index, role, style, perRole);  // a track's role: the same choice as the song's suggestions
    } else {
      // A role no track plays by itself (fx, vox, pad...): the catalogue's samples of that role, the style's first.
      for (const auto& s : index.samples) {
        if (s.info.role != role) continue;
        const bool forStyle = !style.empty() && std::find(s.info.styles.begin(), s.info.styles.end(), style) != s.info.styles.end();
        found.push_back({&s, forStyle ? 60.0 : 50.0, forStyle ? "marked " + styleLabel(style) : (s.info.loop ? "loop" : "one-shot")});
      }
      std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.sample->displayName < b.sample->displayName;
      });
      if ((int)found.size() > perRole) found.resize(std::max(0, perRole));
    }
    if (!found.empty()) out.push_back({role, std::move(found)});
  }
  return out;
}

}  // namespace core::doc
