// The help of every Core plugin (FCore, W-Core, PCore, Stages, the AI's proposals...), one component for all.
// A plugin puts a HelpButton next to its title; a click opens the help window: what the plugin is for, how to use
// it, examples (with "Try it" when the example is a preset), the styles it suits and the library's samples that go
// with it, for the chosen style, each one to hear (and to use on the selected track). The plugin only supplies the
// words: a doc::PluginHelp, written in code or read from help/<id>.json next to Core.exe (pluginHelpFor).
// Contract: docs/contracts/core-plugin-help.md.
//
// A plugin plugs in with three lines:
//   help_.onClick = [this] { showPluginHelp(pluginHelpFor("wave"), [this](const std::string& p) { applyPreset(p); }); };
//   addAndMakeVisible(help_);
//   help_.setBounds(...);  // HelpButton::kSlot x kSlot, right of the plugin's title
#pragma once
#include <functional>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "plugin_help.hpp"
#include "samples.hpp"

namespace core::app {

// The button: a small hollow ring, no letter (the InfoDot rule: the filled grey dot is a tip on hover, this ring
// opens a window on click). Grey at rest, the UI's blue on hover.
class HelpButton final : public juce::Component, public juce::SettableTooltipClient {
public:
  static constexpr float kSize = 9.0f;
  static constexpr int kSlot = 14;  // its hit box: kSlot x kSlot
  HelpButton();
  std::function<void()> onClick;
  void paint(juce::Graphics& g) override;
  void mouseEnter(const juce::MouseEvent&) override { repaint(); }
  void mouseExit(const juce::MouseEvent&) override { repaint(); }
  void mouseUp(const juce::MouseEvent& e) override;
};

// What the window needs from the app, set once by the main window (MainComponent): the library, how a sample is
// heard and used, and the song's style to start from. Every member may be empty (the window then leaves that out).
struct HelpHost {
  std::function<const doc::SampleIndex*()> index;  // null while there is no library
  std::function<bool(const doc::LibrarySample&, const std::string& role)> onPreview;  // heard once, the song stays
  std::function<void(const doc::LibrarySample&)> onUse;  // on the selected track (one undo step)
  std::function<bool()> canUse;                          // a track that plays samples is selected
  std::function<std::string()> songStyle;                // the song's template style ("darkpsycore"...), "" = none
  std::function<void()> onChooseLibrary;                 // no library yet: Samples > Choose the library folder
  juce::Component* centreAround = nullptr;
};
HelpHost& helpHost();

// help/<id>.json next to Core.exe (doc::loadPluginHelp); when it is missing or wrong, a help that says so (title =
// id), never an exception: a plugin without words still opens a window.
doc::PluginHelp pluginHelpFor(const std::string& id);
// The plugins that have a help file (help/*.json next to Core.exe), sorted: Help > How the plugins work.
std::vector<std::string> pluginHelpIds();

// Opens the help window of this plugin (one window at a time: another plugin's help replaces it).
// tryPreset: "Try it" on an example with a preset calls it (empty: no "Try it" buttons).
// style: the style to show samples for first ("" = the song's, else the help's first style).
void showPluginHelp(const doc::PluginHelp& help, std::function<void(const std::string& preset)> tryPreset = {},
                    const std::string& style = "");
void closePluginHelp();

}  // namespace core::app
