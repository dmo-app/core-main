// The help of a Core plugin (FCore, W-Core, PCore, Stages, the AI's proposals...): one small button on the plugin
// opens one window, the same for every plugin, that says in plain words what the plugin is for, how to use it,
// a few examples, the styles it suits and samples of the library that go with it. Each plugin only supplies the
// words (a PluginHelp, in code or as help/<id>.json next to Core.exe); the window is shared (app/desktop
// PluginHelp.h). No GUI here, no audio.
// Contract: docs/contracts/core-plugin-help.md.
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"
#include "samples.hpp"

namespace core::doc {

constexpr const char* kPluginHelpFormat = "core-plugin-help";
constexpr int kPluginHelpVersion = 1;

struct PluginHelp {
  std::string id;       // the plugin ("wave", "fcore", "pcore"...): the file is help/<id>.json
  std::string title;    // "W-Core"
  std::string summary;  // one plain sentence: what it is for
  std::vector<std::string> steps;  // how to use it, one short sentence per step, in order
  struct Example {
    std::string title;   // "A dark, tight bass"
    std::string text;    // what to do and what you will hear
    std::string preset;  // optional: a preset of the plugin the window can apply ("Try it"); "" = none
  };
  std::vector<Example> examples;
  struct Style {
    std::string id;    // as the templates and racks name it: "darkpsycore", "psytrance", "hitech", "slambient"...
    std::string why;   // why the plugin suits it, or how to set it for that style
  };
  std::vector<Style> styles;  // the styles it was made for, best first (a guide: every style still works)
  // The sample roles that go with it (catalogue roles: "bass", "kick", "riser"...): the window shows the library's
  // best samples of each, for the chosen style. Empty: the plugin plays no samples (no samples section).
  std::vector<std::string> sampleRoles;
};

// Read and written as {format, version, id, title, summary, steps, examples: [{title, text, preset?}],
// styles: [{id, why}], sampleRoles}. Throws DocumentError (field: why) for a wrong format, a missing title or
// summary, or a field of the wrong type. Unknown fields are ignored.
PluginHelp pluginHelpFromJson(const json& value);
json pluginHelpToJson(const PluginHelp& help);
// help/<id>.json in this folder; throws DocumentError when it is missing or unusable.
PluginHelp loadPluginHelp(const std::filesystem::path& folder, const std::string& id);

// A style's name on screen: doc::styleLabel (fx_racks.hpp).

// The library's best samples for each of the help's roles in this style ("" = any style), best first, at most
// `perRole` each (doc::suggestSamples). A role with nothing is left out.
struct HelpSamples {
  std::string role;
  std::vector<SampleSuggestion> samples;
};
std::vector<HelpSamples> helpSamples(const SampleIndex& index, const PluginHelp& help, const std::string& style, int perRole = 4);

}  // namespace core::doc
