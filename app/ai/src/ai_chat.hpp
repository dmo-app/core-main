// The AI inside Core (Diogo, 2026-11-16): a chat with Claude that makes beats, the song's structure and changes parts.
// The model only ever proposes: each write tool is one of Core's calls (control/calls.hpp), checked against the song
// as it arrives (validateProposal) and gathered into one proposal; the app shows it in the AI panel and applies it as
// ONE undo step when Diogo presses Apply. Read tools give the song and P-Core's shapes.
//
// No JUCE, no network: the app sends the request bodies this builds (POST https://api.anthropic.com/v1/messages) and
// hands back each response. So the whole conversation is testable without a key.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "calls.hpp"
#include "ai_admit.hpp"
#include "catalog.hpp"
#include "knowledge.hpp"
#include "memory.hpp"
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
  std::string model = "claude-haiku-5-5";  // the default (cost bench 2026-11-16); Sonnet for big requests
  bool pricesSet = false;         // prices given (another provider's); else Anthropic's for anthropic, 0 otherwise
  Prices prices;
  std::string effort = "medium";  // low | medium | high
  int maxTokens = 8000;
  int maxSteps = 8;               // requests per message at most
  double maxUsdPerMessage = 0.25; // a message stops asking the model when it has cost this much (0 = no limit)
  Permissions permissions;        // what the model may do (catalog.hpp): ask, readonly, trusted
  bool gateway = false;           // "ajuda" + "fazer" in place of every edit tool (catalog.hpp: smaller requests)
  bool stream = false;            // Anthropic only: the answer comes as events (stream.hpp); the panel sets it
  // How Core hears two tracks (ler_kick_bass): renders kickId and bassId (their plugins and effects) over the song
  // ticks [start, end) as mono buffers at sampleRate. Returns "" or why not. Not set: ler_kick_bass is not offered.
  std::function<std::string(const std::string& kickId, const std::string& bassId, int64_t start, int64_t end,
                            std::vector<float>& kick, std::vector<float>& bass, double& sampleRate)> listen;
};

// Claude's tool definitions: the catalog's Anthropic view (catalog.hpp), every action offered. The chat itself offers
// only what its permissions do not deny.
json toolsFor(const std::vector<control::CoreFunction>& functions, bool knowledge = false);
// What a request says about a selection (PT-PT); "" when the zone is not active.
std::string zoneBlock(const doc::Document& song, const control::Zone& zone);
// The system prompt (PT-PT) and the style card (the song's P-Core style, its parts, shapes and the song shapes).
// With the permission mode: readonly and trusted add a sentence that says so.
json systemFor(const doc::Document& song, const doc::PCoreCatalog* pcore, const std::string& mode = "ask", bool gateway = false);


// The kind of a call Core left out, from its reason (for the bench and the logs): "name" (a name or option that
// does not exist), "section", "range" (a number outside its range), "missing" (a value not given), "dependency"
// (the call does not fit the track, the master or a new track), "permission", or "other".
std::string problemKind(const std::string& why);

class Chat {
public:
  // With the AI's memory, each request carries the recipes most like it (unless the permissions deny
  // lembrar_receitas), and the model can search for more.
  Chat(Config config, std::vector<control::CoreFunction> functions, const doc::PCoreCatalog* pcore, const Knowledge* knowledge = nullptr,
       const Memory* memory = nullptr);

  // The HTTP request for a request body of ask/onResponse, in the provider's own form.
  HttpRequest http(const json& body) const { return makeRequest(config_.provider, body); }
  double cost(const Usage& u) const;  // at this chat's prices

  // A new message from Diogo: the first request body. The proposal starts empty. With a zone (his selection, calls.hpp
  // Zone): the request says which tracks and bars, ler_musica gives only those tracks whole, and every write is
  // checked against the zone (what falls outside is left out); the proposal carries it, so Apply checks it again.
  json ask(const std::string& text, const doc::Document& song, const control::Zone* zone = nullptr);

  // A response from the API (the provider's own form). Runs the tools (reads; each write is checked and joins the proposal) and returns the
  // next request body, or null when the model is done (or maxSteps was reached, or the response was an error).
  json onResponse(const json& response, const doc::Document& song);

  // What the model said last (its text), and errors (an API error, a cut-off answer).
  const std::string& reply() const { return reply_; }
  const std::string& error() const { return error_; }
  // The proposal so far: {"id", "summary", "calls": [...], "auto"}; empty calls when nothing was proposed. "auto" is
  // true when every call in it is allowed by the permissions (trusted): the app applies it without waiting for Apply.
  json proposal() const;
  // Each write as the model asked it, with Core's verdict ("ok: ..." / "left out: ..."), for the panel.
  const std::vector<std::string>& log() const { return log_; }
  // The recipes of the memory this message's model was given (in the request and by lembrar_receitas).
  const std::vector<std::string>& shown() const { return shown_; }
  // This message as a whole, for the memory (memory.hpp) and, one day, for training a model on what Diogo approved:
  // {"format": "core-ai-episode", "version": 1, provider, model, effort, mode, toolsView ("full" | "gateway"),
  //  "system" (text), "tools" (every action offered, full form: {name, description, inputSchema}), "messages" (the
  //  whole conversation so far, content blocks: text / tool_use {id, name, input} / tool_result {tool_use_id, content}),
  //  "turnStart" (where this message starts in messages), "shown" (the recipes given), "log" (Core's verdict on each write)}.
  json episode() const;
  const Usage& usage() const { return usage_; }   // this message
  const Usage& total() const { return total_; }   // the whole chat
  const Config& config() const { return config_; }
  // The message failed (an API error, no connection): it is taken out of the conversation with what it proposed, so
  // the next message does not carry an instruction that never worked. onResponse does it itself for an API error.
  void abandon();
  // Forget the conversation (the totals stay).
  void reset();
  // The conversation (content blocks), to keep a chat on disk and take it up again later (with any model).
  const json& messages() const { return messages_; }
  // Takes a kept conversation up again: what follows is a new message. A last turn left half way (tool calls never
  // answered, or a request with no answer) is taken out, so the next request is valid.
  void restore(const json& messages);

private:
  json request(const doc::Document& song) const;
  json runTool(const std::string& name, const json& input, const doc::Document& song);
  json withZone(json proposal) const;  // the proposal with this message's zone
  bool overBudget();                   // this message cost maxUsdPerMessage: stop (error_ says so)

  Config config_;
  std::vector<control::CoreFunction> functions_;
  const doc::PCoreCatalog* pcore_;
  const Knowledge* knowledge_;
  const Memory* memory_;
  std::vector<std::string> shown_;
  std::vector<Action> actions_;  // the whole catalog (a denied action is refused when called)
  std::vector<Action> offered_;  // the actions not denied
  json tools_;                   // what is offered: those, or their gateway view
  int asked_ = 0;                // calls in the proposal that wait for Apply
  size_t askStart_ = 0;          // where this message's turn starts in messages_
  json messages_ = json::array();
  json system_;                  // this message's system blocks (set by ask)
  control::Zone zone_;           // this message's selection (inactive = the whole song)
  json calls_ = json::array();
  std::vector<std::string> log_;
  std::string reply_, error_, summary_;
  int steps_ = 0;
  int proposalNo_ = 0;
  Usage usage_, total_;
};

}  // namespace core::ai
