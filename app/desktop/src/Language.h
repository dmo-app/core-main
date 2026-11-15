// The language Core speaks (Settings > General > Language): English, or a language file in lang/ next to
// Core.exe (lang/pt-PT.json: Portuguese). The words are looked up when they are drawn: buttons, labels, menus,
// tooltips, dialogs and the plugin help go through tr() in the LookAndFeel, so most code keeps writing English.
// Custom painting calls tr() itself. Contract: docs/contracts/core-language.md.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace core::app::lang {

// The words for this English text in the chosen language; the English itself when there is none (or in English).
juce::String tr(const juce::String& english);
juce::String tr(const char* english);  // UTF-8

// "en" (the default) or a file of lang/ ("pt-PT"). An unknown code, or a file that cannot be read, is English.
// Repaints every window, so the words change at once.
void use(const juce::String& code);
juce::String current();

struct Choice {
  juce::String code;  // "en", "pt-PT"
  juce::String name;  // in its own words: "English", "Português"
};
std::vector<Choice> choices();  // English first, then each language file found
juce::File folder();             // lang/ next to Core.exe

}  // namespace core::app::lang
