// The models the AI panel offers (Diogo, 2026-11-16: any API, local models too). Claude's three are built in; more
// come from ai-settings.json in %APPDATA%\Core\AI:
//   {"models": [{"name": "Qwen local", "provider": "openai", "baseUrl": "http://localhost:11434/v1", "model": "qwen3:14b"},
//               {"name": "DeepSeek", "provider": "openai", "baseUrl": "https://api.deepseek.com/v1", "model": "deepseek-chat",
//                "apiKeyEnv": "DEEPSEEK_API_KEY", "prices": {"input": 0.27, "output": 1.1, "cacheRead": 0.07}}]}
// A key is never written in the settings: it comes from the environment variable named, or from <keyFile> beside it.
#pragma once
#include <string>
#include <vector>
#include "ai_chat.hpp"

namespace core::ai {

struct ModelEntry {
  std::string name;     // shown
  Config config;        // provider, model, prices (the key is filled in when sending)
  std::string keyEnv;   // ANTHROPIC_API_KEY...; "" = no key (a local server)
  std::string keyFile;  // a file name in the AI folder that may hold the key
};

// Claude Sonnet, Haiku, Opus, then each valid entry of the settings (a bad entry is skipped, with why in problems).
std::vector<ModelEntry> modelEntries(const json& settings, std::vector<std::string>* problems = nullptr);

}  // namespace core::ai
