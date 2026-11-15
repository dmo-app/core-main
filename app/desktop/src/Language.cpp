#include "Language.h"

#include <memory>
#include <string>
#include <unordered_map>
#include "translation.hpp"

namespace core::app::lang {
namespace {

struct State {
  juce::String code = "en";
  std::unique_ptr<doc::Translation> words;
  // Every text asked for, with its words: drawing asks for the same texts many times a second. Cleared when it
  // grows big (texts built from changing numbers) and when the language changes.
  std::unordered_map<std::string, juce::String> seen;
};

State& state() {
  static State s;
  return s;
}

}  // namespace

juce::File folder() { return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("lang"); }

juce::String tr(const juce::String& english) {
  auto& s = state();
  if (s.words == nullptr || english.isEmpty()) return english;
  std::string key = english.toStdString();
  if (const auto it = s.seen.find(key); it != s.seen.end()) return it->second;
  if (s.seen.size() > 20000) s.seen.clear();
  juce::String words = juce::String::fromUTF8(s.words->translate(key).c_str());
  s.seen.emplace(std::move(key), words);
  return words;
}

juce::String tr(const char* english) { return tr(juce::String::fromUTF8(english)); }

void use(const juce::String& code) {
  auto& s = state();
  s.seen.clear();
  s.words.reset();
  s.code = "en";
  if (code.isNotEmpty() && code != "en") {
    const juce::File file = folder().getChildFile(code + ".json");
    try {
      auto t = std::make_unique<doc::Translation>(doc::loadTranslation(file.getFullPathName().toStdString()));
      s.words = std::move(t);
      s.code = code;
    } catch (const std::exception& e) {
      juce::Logger::writeToLog("Language " + code + " not used: " + juce::String::fromUTF8(e.what()));
    }
  }
  for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
    if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i)) w->repaint();
}

juce::String current() { return state().code; }

std::vector<Choice> choices() {
  std::vector<Choice> out{{"en", "English"}};
  for (const auto& f : folder().findChildFiles(juce::File::findFiles, false, "*.json")) {
    try {
      const auto t = doc::loadTranslation(f.getFullPathName().toStdString());
      out.push_back({juce::String::fromUTF8(t.code.c_str()), juce::String::fromUTF8(t.name.c_str())});
    } catch (const std::exception&) {
    }
  }
  return out;
}

}  // namespace core::app::lang
