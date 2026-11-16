// Which model the AI talks to (Diogo, 2026-11-16: any API, not only Claude, and local models one day trained on the
// knowledge base). The chat keeps its conversation in one form (Anthropic's Messages: content blocks text / tool_use /
// tool_result) and each provider turns it into its own request and its answer back:
//   anthropic  https://api.anthropic.com/v1/messages (Claude)
//   openai     any OpenAI-compatible /v1/chat/completions with tools: OpenAI, DeepSeek, OpenRouter, and local servers
//              (Ollama http://localhost:11434/v1, llama.cpp server, LM Studio http://localhost:1234/v1)
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"

namespace core::ai {

using json = nlohmann::json;

struct Provider {
  std::string kind = "anthropic";  // anthropic | openai
  std::string baseUrl;              // "" = the kind's own (https://api.anthropic.com, https://api.openai.com)
  std::string apiKey;               // "" for a local server
  std::string url() const;          // the endpoint
};

struct HttpRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  json body;
};

// One answer, in the chat's own form.
struct Turn {
  json content = json::array();  // blocks: {type: text, text} / {type: tool_use, id, name, input}
  std::string stop;              // end_turn | tool_use | max_tokens | refusal | pause_turn | "" (error)
  json usage = json::object();   // input_tokens, output_tokens, cache_read_input_tokens, cache_creation_input_tokens
  std::string error;             // the API's error, when there is one
};

// `request` is the chat's request (Anthropic form: model, max_tokens, system blocks, tools, messages, output_config).
HttpRequest makeRequest(const Provider& p, const json& request);
Turn readResponse(const Provider& p, const json& response);

}  // namespace core::ai
