// Core in another language (Settings > General > Language). English is the source: every word in the code, the
// help and the racks is written in English, and a language file (lang/<code>.json next to Core.exe) gives each
// English text its words in that language. The app asks for the words when it draws them, so a text with no
// entry simply stays in English. Texts built at run time ("Rises on bars 5-8") match a pattern with "{}" holes.
// No GUI here. Contract: docs/contracts/core-language.md.
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "document.hpp"

namespace core::doc {

constexpr const char* kLanguageFormat = "core-lang";
constexpr int kLanguageVersion = 1;

class Translation {
public:
  std::string code;  // "pt-PT"
  std::string name;  // the language in its own words: "Português"

  // Adds the words for one English text. A "{}" in the English is a hole that matches any text (at least one
  // character); the same holes in the translation take what it matched, in order, or "{1}", "{2}"... by number.
  void add(const std::string& english, const std::string& translated);
  // The words in this language: the exact entry, else the same text without its spaces at the ends, else the
  // first pattern that matches, else the English unchanged.
  std::string translate(std::string_view english) const;
  bool has(std::string_view english) const;  // an entry or a pattern covers it
  std::size_t size() const { return exact_.size() + patterns_.size(); }

private:
  struct Pattern {
    std::vector<std::string> pieces;  // the English between the holes (holes = pieces - 1)
    std::string out;                  // the translation, with its holes
  };
  bool match(const Pattern& p, std::string_view text, std::vector<std::string_view>& holes) const;
  std::unordered_map<std::string, std::string> exact_;
  std::vector<Pattern> patterns_;
};

// Read as {format: "core-lang", version: 1, code, name, strings: {english: words}, same: [english...]}; "same"
// lists texts that stay as they are (names, units) and is only read by the checks. Throws DocumentError (field:
// why) for a wrong format or a field of the wrong type.
Translation translationFromJson(const json& value);
Translation loadTranslation(const std::filesystem::path& file);

}  // namespace core::doc
