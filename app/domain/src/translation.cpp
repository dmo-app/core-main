#include "translation.hpp"

#include <fstream>

namespace core::doc {

void Translation::add(const std::string& english, const std::string& translated) {
  if (english.find("{}") == std::string::npos) {
    exact_[english] = translated;
    return;
  }
  Pattern p;
  std::size_t from = 0;
  for (std::size_t at; (at = english.find("{}", from)) != std::string::npos; from = at + 2) p.pieces.push_back(english.substr(from, at - from));
  p.pieces.push_back(english.substr(from));
  p.out = translated;
  patterns_.push_back(std::move(p));
}

bool Translation::match(const Pattern& p, std::string_view text, std::vector<std::string_view>& holes) const {
  holes.clear();
  const std::string& first = p.pieces.front();
  const std::string& last = p.pieces.back();
  if (text.size() < first.size() + last.size() + (p.pieces.size() - 1)) return false;
  if (text.substr(0, first.size()) != first || text.substr(text.size() - last.size()) != last) return false;
  std::size_t at = first.size();
  const std::size_t end = text.size() - last.size();
  for (std::size_t i = 1; i + 1 < p.pieces.size(); ++i) {  // each middle piece: its first place after a hole of 1+
    const std::string& piece = p.pieces[i];
    const std::size_t found = text.substr(0, end).find(piece, at + 1);
    if (found == std::string_view::npos) return false;
    holes.push_back(text.substr(at, found - at));
    at = found + piece.size();
  }
  if (end <= at) return false;
  holes.push_back(text.substr(at, end - at));
  return true;
}

std::string Translation::translate(std::string_view english) const {
  if (english.empty()) return {};
  if (const auto it = exact_.find(std::string(english)); it != exact_.end()) return it->second;
  const auto a = english.find_first_not_of(" \t\n");
  if (a == std::string_view::npos) return std::string(english);
  const auto b = english.find_last_not_of(" \t\n");
  const std::string_view core = english.substr(a, b - a + 1);
  if (core.size() != english.size()) {
    if (const auto it = exact_.find(std::string(core)); it != exact_.end())
      return std::string(english.substr(0, a)) + it->second + std::string(english.substr(b + 1));
  }
  std::vector<std::string_view> holes;
  for (const auto& p : patterns_) {
    if (!match(p, english, holes)) continue;
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < p.out.size(); ++i) {
      if (p.out[i] == '{' && i + 1 < p.out.size()) {
        if (p.out[i + 1] == '}') {
          if (next < holes.size()) out += holes[next++];
          ++i;
          continue;
        }
        if (p.out[i + 1] >= '1' && p.out[i + 1] <= '9' && i + 2 < p.out.size() && p.out[i + 2] == '}') {
          const std::size_t n = (std::size_t)(p.out[i + 1] - '1');
          if (n < holes.size()) out += holes[n];
          i += 2;
          continue;
        }
      }
      out += p.out[i];
    }
    return out;
  }
  return std::string(english);
}

bool Translation::has(std::string_view english) const {
  if (exact_.count(std::string(english))) return true;
  std::vector<std::string_view> holes;
  for (const auto& p : patterns_)
    if (match(p, english, holes)) return true;
  return false;
}

Translation translationFromJson(const json& value) {
  if (!value.is_object() || value.value("format", "") != kLanguageFormat) throw DocumentError("format", "not a Core language file");
  if (!value.contains("version") || !value["version"].is_number_integer() || value["version"].get<int64_t>() != kLanguageVersion)
    throw DocumentError("version", "this Core reads language files of version 1");
  Translation t;
  if (!value.contains("code") || !value["code"].is_string()) throw DocumentError("code", "the language's code is missing");
  t.code = value["code"].get<std::string>();
  t.name = value.value("name", t.code);
  if (value.contains("strings")) {
    const json& strings = value["strings"];
    if (!strings.is_object()) throw DocumentError("strings", "must be an object of English text: words");
    for (auto it = strings.begin(); it != strings.end(); ++it) {
      if (!it.value().is_string()) throw DocumentError("strings." + it.key(), "must be text");
      if (!it.value().get<std::string>().empty()) t.add(it.key(), it.value().get<std::string>());
    }
  }
  return t;
}

Translation loadTranslation(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw DocumentError("file", "cannot read " + file.string());
  json value;
  try {
    value = json::parse(in);
  } catch (const std::exception& e) {
    throw DocumentError("file", std::string("not valid JSON: ") + e.what());
  }
  return translationFromJson(value);
}

}  // namespace core::doc
