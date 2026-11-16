// The AI inside Core (Diogo, 2026-11-16): a chat with Claude that makes beats, the song's structure and changes parts.
// The model only ever proposes: each write tool is one of Core's calls (control/calls.hpp), checked against the song
// as it arrives (validateProposal) and gathered into one proposal; the app shows it in the AI panel and applies it as
// ONE undo step when Diogo presses Apply. Read tools give the song and P-Core's shapes.
//
// No JUCE, no network: the app sends the request bodies this builds (POST https://api.anthropic.com/v1/messages) and
// hands back each response. So the whole conversation is testable without a key.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "calls.hpp"
#include "knowledge.hpp"
#include "pcore.hpp"
#include "provider.hpp"

namespace core::ai {

using json = nlohmann::json;

// Prices per million tokens (Anthropic's table, 2026-11-03). Cache writes 1.25x input (5 minutes).
struct Prices { double input = 0, output = 0, cacheWrite = 0, cacheRead = 0; };
Prices pricesOf(const std::string& model);  // unknown model: Sonnet 5.5's

struct Usage {
  int64_t input = 0, output = 0, cacheWrite = 0, cacheRead = 0;
  int requests = 0;
  void add(const json& usage);                       // a response's "usage"
  double costUsd(const std::string& model) const;    // at Anthropic's prices
  double costUsd(const Prices& prices) const;
};

struct Config {
  Provider provider;              // anthropic (Claude), or any OpenAI-compatible API / local server
  std::string model = "claude-sonnet-5-5";
  bool pricesSet = false;         // prices given (another provider's); else Anthropic's for anthropic, 0 otherwise
  Prices prices;
  std::string effort = "medium";  // low | medium | high
  int maxTokens = 8000;
  int maxSteps = 8;               // requests per message at most
};

// The tool name of a Core function ("pcore", "fx_reverb"...): letters, digits, _ and -.
std::string toolName(const std::string& functionId);
// Claude's tool definitions: the read tools, then one per function Core can run (its text choices as enums, its knobs
// with their ranges). The last one carries cache_control (tools and system are cached together). With the knowledge
// base, "consultar_manual" too.
json toolsFor(const std::vector<control::CoreFunction>& functions, bool knowledge = false);
// The system prompt (PT-PT) and the style card (the song's P-Core style, its parts, shapes and the song shapes).
json systemFor(const doc::Document& song, const doc::PCoreCatalog* pcore);

class Chat {
public:
  Chat(Config config, std::vector<control::CoreFunction> functions, const doc::PCoreCatalog* pcore, const Knowledge* knowledge = nullptr);

  // The HTTP request for a request body of ask/onResponse, in the provider's own form.
  HttpRequest http(const json& body) const { return makeRequest(config_.provider, body); }
  double cost(const Usage& u) const;  // at this chat's prices

  // A new message from Diogo: the first request body. The proposal starts empty.
  json ask(const std::string& text, const doc::Document& song);

  // A response from the API (the provider's own form). Runs the tools (reads; each write is checked and joins the proposal) and returns the
  // next request body, or null when the model is done (or maxSteps was reached, or the response was an error).
  json onResponse(const json& response, const doc::Document& song);

  // What the model said last (its text), and errors (an API error, a cut-off answer).
  const std::string& reply() const { return reply_; }
  const std::string& error() const { return error_; }
  // The proposal so far: {"id", "summary", "calls": [...]}; empty calls when nothing was proposed.
  json proposal() const;
  // Each write as the model asked it, with Core's verdict ("ok: ..." / "left out: ..."), for the panel.
  const std::vector<std::string>& log() const { return log_; }
  const Usage& usage() const { return usage_; }   // this message
  const Usage& total() const { return total_; }   // the whole chat
  const Config& config() const { return config_; }
  // Forget the conversation (the totals stay).
  void reset();

private:
  json request(const doc::Document& song) const;
  json runTool(const std::string& name, const json& input, const doc::Document& song);

  Config config_;
  std::vector<control::CoreFunction> functions_;
  const doc::PCoreCatalog* pcore_;
  const Knowledge* knowledge_;
  json tools_;
  json messages_ = json::array();
  json calls_ = json::array();
  std::vector<std::string> log_;
  std::string reply_, error_, summary_;
  int steps_ = 0;
  int proposalNo_ = 0;
  Usage usage_, total_;
};

}  // namespace core::ai
