#include "models.hpp"

namespace core::ai {

std::vector<ModelEntry> modelEntries(const json& settings, std::vector<std::string>* problems) {
  std::vector<ModelEntry> out;
  for (const auto& [id, name] : std::vector<std::pair<const char*, const char*>>{
           {"claude-haiku-5-5", "Claude Haiku 5.5"}, {"claude-sonnet-5-5", "Claude Sonnet 5.5 (big requests)"}, {"claude-opus-5-5", "Claude Opus 5.5"}}) {
    ModelEntry e;
    e.name = name;
    e.config.model = id;
    e.keyEnv = "ANTHROPIC_API_KEY";
    e.keyFile = "anthropic-key.txt";
    out.push_back(e);
  }
  if (!settings.is_object() || !settings.contains("models") || !settings["models"].is_array()) return out;
  for (const auto& m : settings["models"]) {
    auto bad = [&](const std::string& why) { if (problems) problems->push_back(why); };
    if (!m.is_object()) { bad("a model entry is not an object"); continue; }
    ModelEntry e;
    e.name = m.value("name", std::string());
    e.config.model = m.value("model", std::string());
    e.config.provider.kind = m.value("provider", std::string("openai"));
    e.config.provider.baseUrl = m.value("baseUrl", std::string());
    e.keyEnv = m.value("apiKeyEnv", std::string());
    e.keyFile = m.value("apiKeyFile", std::string());
    if (e.config.model.empty()) { bad("a model entry has no \"model\""); continue; }
    if (e.config.provider.kind != "openai" && e.config.provider.kind != "anthropic") { bad(e.config.model + ": provider must be openai or anthropic"); continue; }
    if (e.config.provider.kind == "openai" && e.config.provider.baseUrl.empty()) { bad(e.config.model + ": an openai provider needs \"baseUrl\""); continue; }
    if (e.keyFile.find_first_of("/\\:") != std::string::npos) { bad(e.config.model + ": apiKeyFile is a file name in the AI folder"); continue; }
    if (e.name.empty()) e.name = e.config.model;
    if (m.contains("prices") && m["prices"].is_object()) {
      const json& p = m["prices"];
      e.config.pricesSet = true;
      e.config.prices = {p.value("input", 0.0), p.value("output", 0.0), p.value("cacheWrite", p.value("input", 0.0)), p.value("cacheRead", p.value("input", 0.0))};
    }
    if (m.contains("maxTokens") && m["maxTokens"].is_number_integer()) e.config.maxTokens = m["maxTokens"].get<int>();
    out.push_back(e);
  }
  return out;
}

}  // namespace core::ai
