// The AI inside Core (2026-11-16): the song's calls (app/control/song_calls), the chat with its tools and costs
// (app/ai/ai_chat), the local knowledge base, the providers and the models (app/ai). Canned answers, no network.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include "ai_chat.hpp"
#include "calls.hpp"
#include "catalog.hpp"
#include "edit.hpp"
#include "knowledge.hpp"
#include "memory.hpp"
#include "models.hpp"
#include "pcore.hpp"
#include "provider.hpp"
#include "score.hpp"
#include "support/check.hpp"
#include "stream.hpp"
#include "synth.hpp"
#include "synth_roles.hpp"

using namespace core;
using json = nlohmann::json;

namespace {
const doc::PCoreCatalog& catalog() {
  static const doc::PCoreCatalog c = doc::loadPCoreCatalog(std::string(CORE_SOURCE_DIR) + "/pcore/styles.json");
  return c;
}

// The knowledge base, built once from the JSONL into a temporary file.
const std::filesystem::path& knowledgeDb() {
  static const std::filesystem::path db = [] {
    const auto p = std::filesystem::temp_directory_path() / "core-ai-knowledge-test.sqlite";
    std::filesystem::remove(p);
    ai::buildKnowledge(std::string(CORE_SOURCE_DIR) + "/ai/knowledge/core-manual.jsonl", p);  // "knowledge base" checks it
    return p;
  }();
  return db;
}
}  // namespace

// The song's own calls (control/song_calls.hpp): checked by validateProposal, made by applyProposal.


namespace song_calls_t {

const doc::Track* named(const doc::Document& d, const std::string& name) {
  for (const auto& t : d.tracks)
    if (t.name == name) return &t;
  return nullptr;
}

int notesOf(const doc::Track& t) {
  int n = 0;
  for (const auto& p : t.patterns) n += (int)p.notes.size();
  return n;
}

bool mentions(const std::vector<std::string>& lines, const std::string& word) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(word) != std::string::npos; });
}

// A beat from nothing: a song shape, two new tracks, P-Core writes kick and bass together, S-Core arranges.
void beatFromNothing(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  song.name = "Dark psycore test";
  const json raw = {{"id", "t1"}, {"summary", "a beat"}, {"calls", json::array({
      {{"fn", "shape"}, {"shape", "darkpsycore-64"}},
      {{"fn", "track"}, {"part", "kick"}, {"name", "Kick"}},
      {{"fn", "track"}, {"part", "bass"}},
      {{"fn", "pcore"}, {"on", "Kick"}, {"section", "Drop"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}},
      {{"fn", "pcore"}, {"on", "Bass"}, {"section", "Drop"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}},
      {{"fn", "arrange"}}})}};
  const control::Proposal p = control::validateProposal(raw, fns, song, nullptr);
  for (const auto& pr : p.problems) std::cerr << "problem: " << pr << "\n";
  CHECK(p.problems.empty());
  CHECK_EQ(p.calls.size(), (size_t)6);
  CHECK(!p.needsTrack());

  control::ApplyEnv env;
  env.pcore = &cat;
  const auto lines = control::applyProposal(song, p, "", env);
  CHECK_EQ(lines.size(), (size_t)6);
  CHECK_EQ(song.sections.size(), (size_t)6);
  CHECK(!song.sections.empty() && song.sections[2].name == "Drop");
  const doc::Track* kick = named(song, "Kick");
  const doc::Track* bass = named(song, "Bass");
  CHECK(kick && bass);
  if (!kick || !bass) return;
  CHECK(notesOf(*kick) > 0);
  CHECK(notesOf(*bass) > 0);
  // The bass leaves the kick's ticks free (written together).
  int clash = 0;
  for (const auto& k : kick->patterns.front().notes)
    for (const auto& b : bass->patterns.front().notes)
      if (k.tick == b.tick) ++clash;
  CHECK_EQ(clash, 0);
  // Arranged: the kick plays in the drop and rests in the break.
  CHECK(doc::scorePlays(*kick, song.sections[2]));
  CHECK(!doc::scorePlays(*kick, song.sections[3]));
  CHECK(mentions(lines, "Song shape"));
}

// Changing a part: the hats rest in the drop, a new take of the bass only there.
void changeAPart(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newTemplateDocument();
  doc::setScoreShape(song, "darkpsycore-64");
  std::vector<std::string> ids;
  for (const auto& t : song.tracks) ids.push_back(t.id);
  doc::scoreArrange(song, ids);
  const doc::Track* hats = named(song, "Hats");
  CHECK(hats != nullptr);
  if (!hats) return;
  CHECK(doc::scorePlays(*hats, song.sections[2]));
  const json raw = {{"calls", json::array({{{"fn", "plays"}, {"on", "Hats"}, {"section", "drop"}, {"play", "off"}},
                                           {{"fn", "pcore"}, {"on", "Bass"}, {"section", "Drop"}, {"mode", "alter"}, {"args", {{"density", -30}}}}})}};
  const control::Proposal p = control::validateProposal(raw, fns, song, nullptr);
  for (const auto& pr : p.problems) std::cerr << "problem: " << pr << "\n";
  CHECK(p.problems.empty());
  control::ApplyEnv env;
  env.pcore = &cat;
  control::applyProposal(song, p, "", env);
  CHECK(!doc::scorePlays(*named(song, "Hats"), song.sections[2]));
  CHECK(doc::scorePlays(*named(song, "Hats"), song.sections[4]));
}

// What is left out, and why.
void leftOut(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  const doc::Document song = doc::newTemplateDocument();
  auto problemsOf = [&](const json& call) {
    return control::validateProposal({{"calls", json::array({call})}}, fns, song, nullptr);
  };
  CHECK_EQ(problemsOf({{"fn", "shape"}, {"shape", "polka-12"}}).calls.size(), (size_t)0);
  CHECK_EQ(problemsOf({{"fn", "plays"}, {"on", "Kick"}, {"section", "Drop"}, {"play", "on"}}).calls.size(), (size_t)0);  // no sections yet
  {  // "build" is the riser part (the Haiku asked for it in the benches)
    const auto p = problemsOf({{"fn", "track"}, {"part", "Build"}, {"name", "Build-up"}});
    CHECK(p.problems.empty() && p.calls.size() == 1);
    if (p.calls.size() == 1) CHECK_EQ(p.calls[0].texts.at("part"), std::string("riser"));
  }
  {  // the reason says what to do: shape first (the error the Haiku met in the benches)
    const auto p = problemsOf({{"fn", "pcore"}, {"on", "Kick"}, {"section", "Drop"}});
    CHECK(p.calls.empty() && !p.problems.empty() && p.problems[0].find("call shape first") != std::string::npos);
    if (!p.problems.empty()) CHECK_EQ(ai::problemKind(p.problems[0]), std::string("section"));
  }
  CHECK_EQ(problemsOf({{"fn", "pcore"}, {"section", "Drop"}}).calls.size(), (size_t)0);  // no track
  CHECK_EQ(problemsOf({{"fn", "pcore"}, {"on", "Kick"}, {"args", {{"variationEvery", 3}}}}).calls.size(), (size_t)0);
  CHECK_EQ(problemsOf({{"fn", "pcore"}, {"on", "Kick"}, {"args", {{"bars", 65}}}}).calls.size(), (size_t)0);
  CHECK_EQ(problemsOf({{"fn", "pcore"}, {"on", "Kick"}, {"shape", "no-such-shape"}}).calls.size(), (size_t)0);
  CHECK_EQ(problemsOf({{"fn", "track"}}).calls.size(), (size_t)0);  // no part
  CHECK_EQ(problemsOf({{"fn", "pcore"}, {"on", "master"}}).calls.size(), (size_t)0);
  // A track name already there: the new one gets a number, with a warning.
  const control::Proposal again = problemsOf({{"fn", "track"}, {"part", "kick"}, {"name", "Kick"}});
  CHECK_EQ(again.calls.size(), (size_t)1);
  CHECK(!again.calls.empty() && again.calls[0].trackName == "Kick 2");
  CHECK(!again.warnings().empty());
}

// A refused call changes nothing: the app commits only what applied whole.
void refusedChangesNothing(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newTemplateDocument();
  const control::Proposal p = control::validateProposal({{"calls", json::array({{{"fn", "pcore"}, {"on", "Kick"}}})}}, fns, song, nullptr);
  CHECK_EQ(p.calls.size(), (size_t)1);
  bool threw = false;
  try {
    control::applyProposal(song, p, "", control::ApplyEnv{});  // no catalogue
  } catch (const doc::DocumentError&) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace song_calls_t

// The AI chat (app/ai): requests, tools, the proposal it gathers and the cost, with canned answers (no network).


namespace ai_chat_t {

json toolUse(const std::string& id, const std::string& name, const json& input) {
  return {{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}};
}
json answer(const json& content, const std::string& stop, int in = 1000, int out = 200, int cacheRead = 0, int cacheWrite = 0) {
  return {{"type", "message"}, {"role", "assistant"}, {"content", content}, {"stop_reason", stop},
          {"usage", {{"input_tokens", in}, {"output_tokens", out}, {"cache_read_input_tokens", cacheRead}, {"cache_creation_input_tokens", cacheWrite}}}};
}
json lastToolResult(const json& request) {
  const json& m = request["messages"].back();
  return json::parse(m["content"].back()["content"].get<std::string>());
}

void tools(const doc::PCoreCatalog& cat) {
  const json t = ai::toolsFor(control::buildFunctions(&cat));
  CHECK(t.is_array() && t.size() > 5);
  bool hasPcore = false, hasRead = false;
  for (const auto& x : t) {
    const std::string n = x["name"];
    CHECK(n.size() <= 64);
    for (char c : n) CHECK(std::isalnum((unsigned char)c) || c == '_' || c == '-');
    if (n == "pcore") {
      hasPcore = true;
      CHECK(x["input_schema"]["properties"]["shape"]["enum"].size() == cat.shapes.size());
      CHECK(x["input_schema"]["properties"].contains("args"));
    }
    if (n == "ler_musica") hasRead = true;
  }
  CHECK(hasPcore && hasRead);
  CHECK(t.back().contains("cache_control"));
  CHECK_EQ(ai::toolName("fx:reverb"), std::string("fx_reverb"));
}

void conversation(const doc::PCoreCatalog& cat) {
  doc::Document song = doc::newEmptyDocument();
  song.name = "Dark psycore";
  ai::Chat chat(ai::Config{}, control::buildFunctions(&cat), &cat);
  json req = chat.ask("Faz uma batida no drop", song);
  CHECK_EQ(req["model"].get<std::string>(), std::string("claude-haiku-5-5"));
  CHECK(req["system"].size() == 2 && req["system"][1].contains("cache_control"));
  CHECK_EQ(req["messages"].size(), (size_t)1);
  CHECK_EQ(req["output_config"]["effort"].get<std::string>(), std::string("medium"));

  // 1: reads the song and P-Core's kick shapes.
  req = chat.onResponse(answer(json::array({toolUse("a", "ler_musica", json::object()), toolUse("b", "formas_pcore", {{"part", "kick"}})}), "tool_use", 4000, 100, 0, 4000), song);
  CHECK(!req.is_null());
  CHECK(lastToolResult(req).contains("shapes"));
  // 2: proposes; one call is wrong and is left out with the reason.
  req = chat.onResponse(answer(json::array({
      toolUse("c", "shape", {{"shape", "darkpsycore-64"}}),
      toolUse("d", "track", {{"part", "kick"}, {"name", "Kick"}}),
      toolUse("e", "pcore", {{"on", "Kick"}, {"section", "Drop"}, {"args", {{"bars", 4}}}}),
      toolUse("f", "pcore", {{"on", "Kick"}, {"section", "Chorus"}})}), "tool_use", 300, 400, 4000), song);
  CHECK(!req.is_null());
  const json bad = lastToolResult(req);
  CHECK_EQ(bad["result"].get<std::string>(), std::string("left out"));
  CHECK(!bad["problems"].empty());
  CHECK_EQ(chat.proposal()["calls"].size(), (size_t)3);
  // 3: done.
  req = chat.onResponse(answer(json::array({{{"type", "text"}, {"text", "Propus a forma, um kick e o padrão do drop."}}}), "end_turn", 300, 60, 4300), song);
  CHECK(req.is_null());
  CHECK(chat.error().empty());
  CHECK_EQ(chat.reply(), std::string("Propus a forma, um kick e o padrão do drop."));
  CHECK_EQ(chat.proposal()["summary"].get<std::string>(), chat.reply());
  CHECK_EQ(chat.log().size(), (size_t)4);

  // The proposal applies as Core applies any other.
  const auto fns = control::buildFunctions(&cat);
  const control::Proposal p = control::validateProposal(chat.proposal(), fns, song, nullptr);
  CHECK(p.problems.empty());
  control::ApplyEnv env;
  env.pcore = &cat;
  doc::Document after = song;
  control::applyProposal(after, p, "", env);
  CHECK_EQ(after.tracks.size(), (size_t)1);
  CHECK(!after.tracks.empty() && !after.tracks[0].patterns.front().notes.empty());

  // The cost: 3 requests on Sonnet 5.5.
  const ai::Usage& u = chat.usage();
  CHECK_EQ(u.requests, 3);
  CHECK_EQ(u.input, (int64_t)4600);
  CHECK_EQ(u.cacheRead, (int64_t)8300);
  CHECK_EQ(u.cacheWrite, (int64_t)4000);
  const double want = (4600 * 2.0 + 560 * 10.0 + 4000 * 2.5 + 8300 * 0.1) / 1e6;
  CHECK(std::fabs(u.costUsd("claude-sonnet-5-5") - want) < 1e-9);
  CHECK(u.costUsd("claude-haiku-5-5") < u.costUsd("claude-sonnet-5-5"));

  // A new message starts a new proposal; the chat remembers the conversation and the totals.
  req = chat.ask("Tira o kick do drop", song);
  CHECK(chat.proposal()["calls"].empty());
  CHECK(req["messages"].size() == 7);
  CHECK_EQ(chat.total().requests, 3);
}

void errorsAndLimits(const doc::PCoreCatalog& cat) {
  doc::Document song = doc::newTemplateDocument();
  ai::Config cfg;
  cfg.maxSteps = 2;
  ai::Chat chat(cfg, control::buildFunctions(&cat), &cat);
  chat.ask("olá", song);
  CHECK(chat.onResponse({{"type", "error"}, {"error", {{"type", "authentication_error"}, {"message", "invalid x-api-key"}}}}, song).is_null());
  CHECK(chat.error().find("authentication_error") != std::string::npos);
  chat.ask("lê a música", song);
  json req = chat.onResponse(answer(json::array({toolUse("a", "ler_musica", json::object())}), "tool_use"), song);
  CHECK(!req.is_null());
  CHECK(lastToolResult(req)["tracks"].size() == song.tracks.size());
  req = chat.onResponse(answer(json::array({toolUse("b", "ler_musica", json::object())}), "tool_use"), song);
  CHECK(req.is_null());
  CHECK(chat.error().find("stopped") != std::string::npos);
  chat.ask("outra", song);
  CHECK(chat.onResponse(answer(json::array({{{"type", "text"}, {"text", "..."}}}), "max_tokens"), song).is_null());
  CHECK(chat.error().find("max_tokens") != std::string::npos);
}

}  // namespace ai_chat_t

// The AI's local knowledge base (app/ai/knowledge) and the providers (app/ai/provider).


namespace ai_knowledge_t {

void knowledgeBase(const std::filesystem::path& db) {
  std::string why;
  const int n = ai::buildKnowledge(std::string(CORE_SOURCE_DIR) + "/ai/knowledge/core-manual.jsonl", db, &why);
  if (n < 0) std::cerr << why << "\n";
  CHECK(n > 50);
  auto k = ai::Knowledge::open(db, &why);
  CHECK(k != nullptr);
  if (!k) return;
  CHECK_EQ(k->size(), n);
  CHECK(k->from().rfind("dmo-app/core-manual@", 0) == 0);
  const auto hits = k->search("KBBB bass kick dark psycore", "darkpsycore", 4);
  CHECK(!hits.empty() && hits.size() <= 4);
  CHECK(!hits.empty() && (hits[0].style == "darkpsycore" || hits[0].style.empty()));
  // Accents fold: "energia" and "energía"-like spellings, Portuguese words, operators never get through.
  CHECK(!k->search("arranjo energia drop", "", 3).empty());
  CHECK(k->search("\"\" ( * ) :", "", 3).empty());          // punctuation only: nothing to search
  CHECK(k->search("kick\" OR NEAR(bass", "", 3).size() <= 3);  // FTS operators are plain words, never syntax
  CHECK(k->search("", "", 3).empty());
  CHECK(ai::Knowledge::open(db.parent_path() / "missing.sqlite") == nullptr);
}

void consultTool(const std::filesystem::path& db) {
  auto k = ai::Knowledge::open(db);
  const doc::PCoreCatalog cat = doc::loadPCoreCatalog(std::string(CORE_SOURCE_DIR) + "/pcore/styles.json");
  const doc::Document song = doc::newTemplateDocument();
  ai::Chat chat(ai::Config{}, control::buildFunctions(&cat), &cat, k.get());
  json req = chat.ask("como faço o baixo?", song);
  bool has = false;
  for (const auto& t : req["tools"]) has |= t["name"] == "consultar_manual";
  CHECK(has);
  req = chat.onResponse({{"content", json::array({{{"type", "tool_use"}, {"id", "a"}, {"name", "consultar_manual"}, {"input", {{"pergunta", "KBBB bass"}}}}})},
                         {"stop_reason", "tool_use"}, {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}}},
                        song);
  const json out = json::parse(req["messages"].back()["content"][0]["content"].get<std::string>());
  CHECK(out.contains("excerpts") && !out["excerpts"].empty());
}

void openAiProvider() {
  ai::Provider p;
  p.kind = "openai";
  p.baseUrl = "http://localhost:11434/v1/";
  CHECK_EQ(p.url(), std::string("http://localhost:11434/v1/chat/completions"));
  const json request = {{"model", "qwen3"},
                        {"max_tokens", 100},
                        {"system", json::array({{{"type", "text"}, {"text", "S"}}})},
                        {"tools", json::array({{{"name", "pcore"}, {"description", "d"}, {"input_schema", {{"type", "object"}}}}})},
                        {"messages", json::array({{{"role", "user"}, {"content", "olá"}},
                                                  {{"role", "assistant"}, {"content", json::array({{{"type", "text"}, {"text", "vou ler"}},
                                                                                                    {{"type", "tool_use"}, {"id", "t1"}, {"name", "ler_musica"}, {"input", json::object()}}})}},
                                                  {{"role", "user"}, {"content", json::array({{{"type", "tool_result"}, {"tool_use_id", "t1"}, {"content", "{}"}}})}}})}};
  const ai::HttpRequest h = ai::makeRequest(p, request);
  CHECK_EQ(h.body["messages"].size(), (size_t)4);
  CHECK_EQ(h.body["messages"][0]["role"].get<std::string>(), std::string("system"));
  CHECK_EQ(h.body["messages"][2]["tool_calls"][0]["function"]["name"].get<std::string>(), std::string("ler_musica"));
  CHECK_EQ(h.body["messages"][3]["role"].get<std::string>(), std::string("tool"));
  CHECK_EQ(h.body["tools"][0]["function"]["name"].get<std::string>(), std::string("pcore"));
  for (const auto& [k, v] : h.headers) CHECK(k != "Authorization");  // no key: a local server
  const ai::Turn t = ai::readResponse(p, {{"choices", json::array({{{"finish_reason", "tool_calls"},
                                                                     {"message", {{"content", nullptr},
                                                                                  {"tool_calls", json::array({{{"id", "c1"}, {"type", "function"},
                                                                                                               {"function", {{"name", "shape"}, {"arguments", "{\"shape\":\"darkpsycore-64\"}"}}}}})}}}}})},
                                          {"usage", {{"prompt_tokens", 100}, {"completion_tokens", 20}, {"prompt_tokens_details", {{"cached_tokens", 60}}}}}});
  CHECK_EQ(t.stop, std::string("tool_use"));
  CHECK_EQ(t.content[0]["input"]["shape"].get<std::string>(), std::string("darkpsycore-64"));
  CHECK_EQ(t.usage["input_tokens"].get<int64_t>(), (int64_t)40);
  CHECK(!ai::readResponse(p, {{"error", {{"message", "model not found"}}}}).error.empty());
  // A local model costs nothing unless prices are given.
  ai::Config cfg;
  cfg.provider = p;
  cfg.model = "qwen3";
  const doc::PCoreCatalog cat = doc::loadPCoreCatalog(std::string(CORE_SOURCE_DIR) + "/pcore/styles.json");
  ai::Chat chat(cfg, control::buildFunctions(&cat), &cat);
  ai::Usage u;
  u.input = 1000000;
  CHECK_EQ(chat.cost(u), 0.0);
  ai::Provider claude;
  claude.apiKey = "k";
  const ai::HttpRequest c = ai::makeRequest(claude, request);
  CHECK_EQ(c.url, std::string("https://api.anthropic.com/v1/messages"));
  CHECK(c.body == request);
}

void models() {
  std::vector<std::string> problems;
  const json settings = {{"models", json::array({{{"name", "Qwen local"}, {"provider", "openai"}, {"baseUrl", "http://localhost:11434/v1"}, {"model", "qwen3:14b"}},
                                                 {{"model", "deepseek-chat"}, {"baseUrl", "https://api.deepseek.com/v1"}, {"apiKeyEnv", "DEEPSEEK_API_KEY"},
                                                  {"prices", {{"input", 0.27}, {"output", 1.1}}}},
                                                 {{"name", "no model"}},
                                                 {{"model", "x"}, {"provider", "openai"}},
                                                 {{"model", "y"}, {"provider", "anthropic"}, {"apiKeyFile", "..\\secret.txt"}}})}};
  const auto list = ai::modelEntries(settings, &problems);
  CHECK_EQ(list.size(), (size_t)5);
  CHECK_EQ(problems.size(), (size_t)3);
  CHECK(list.size() == 5 && list[0].config.provider.kind == "anthropic" && list[0].keyEnv == "ANTHROPIC_API_KEY");
  CHECK(list.size() == 5 && list[3].name == "Qwen local" && list[3].keyEnv.empty() && !list[3].config.pricesSet);
  CHECK(list.size() == 5 && list[4].config.pricesSet && list[4].config.prices.output == 1.1);
  CHECK_EQ(ai::modelEntries(nullptr).size(), (size_t)3);
}

}  // namespace ai_knowledge_t

// The one catalog (pattern from theDAW's tool_catalog.py / permissions.py): every provider's tools are views of it, and
// one policy decides allow / deny / ask.
namespace catalog_t {

using ai_chat_t::answer;
using ai_chat_t::lastToolResult;
using ai_chat_t::toolUse;

void views(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  const std::vector<ai::Action> acts = ai::catalog(fns, true);
  const json anthropic = ai::anthropicTools(acts), openai = ai::openAiTools(acts), mcp = ai::mcpTools(acts);
  CHECK_EQ(anthropic.size(), acts.size());
  CHECK_EQ(openai.size(), acts.size());
  CHECK_EQ(mcp.size(), acts.size());
  for (size_t i = 0; i < acts.size(); ++i) {
    CHECK_EQ(anthropic[i]["name"].get<std::string>(), acts[i].name);
    CHECK(openai[i]["function"]["parameters"] == acts[i].schema);
    CHECK(mcp[i]["inputSchema"] == acts[i].schema);
    CHECK(acts[i].kind == ai::Kind::read ? acts[i].function.empty() : !acts[i].function.empty());
  }
  // Names are unique, and the chat's tools are the same view.
  std::set<std::string> seen;
  for (const auto& a : acts) CHECK(seen.insert(a.name).second);
  CHECK(ai::toolsFor(fns, true) == anthropic);
  // What the OpenAI-compatible provider sends is the catalog's OpenAI view.
  ai::Config c;
  c.provider.kind = "openai";
  ai::Chat chat(c, fns, &cat);
  const ai::HttpRequest h = chat.http(chat.ask("olá", doc::newEmptyDocument()));
  CHECK(h.body["tools"] == ai::openAiTools(ai::catalog(fns, false)));
  // Without the knowledge base there is no consultar_manual.
  CHECK(ai::findAction(ai::catalog(fns, false), "consultar_manual") == nullptr);
  CHECK(ai::findAction(acts, "consultar_manual") != nullptr);
}

void decide(const doc::PCoreCatalog& cat) {
  const std::vector<ai::Action> acts = ai::catalog(control::buildFunctions(&cat), false);
  const ai::Action& read = *ai::findAction(acts, "ler_musica");
  const ai::Action& pcore = *ai::findAction(acts, "pcore");
  const ai::Action& shape = *ai::findAction(acts, "shape");
  ai::Permissions p;  // ask
  CHECK(ai::decide(p, read) == ai::Verdict::allow);
  CHECK(ai::decide(p, pcore) == ai::Verdict::ask);
  p.mode = "trusted";
  CHECK(ai::decide(p, pcore) == ai::Verdict::allow);
  p.ask = {"shape"};  // per action, over the mode
  CHECK(ai::decide(p, shape) == ai::Verdict::ask);
  p.deny = {"pcore"};
  CHECK(ai::decide(p, pcore) == ai::Verdict::deny);
  p.allow = {"pcore"};  // deny wins
  CHECK(ai::decide(p, pcore) == ai::Verdict::deny);
  p = {};
  p.deny = {"*"};
  CHECK(ai::decide(p, read) == ai::Verdict::deny);
  p = {};
  p.mode = "readonly";
  p.allow = {"pcore"};  // read-only wins over everything
  CHECK(ai::decide(p, pcore) == ai::Verdict::deny);
  CHECK(ai::decide(p, read) == ai::Verdict::allow);
  // A family names every action of it.
  p = {};
  p.deny = {"pcore"};
  CHECK(pcore.family == "pcore");
  CHECK(ai::decide(p, pcore) == ai::Verdict::deny);

  // From ai-settings.json; bad values are left out with why.
  std::vector<std::string> problems;
  ai::Permissions s = ai::permissionsFrom(json::parse(R"({"permissions": {"mode": "trusted", "deny": ["mix"], "ask": ["shape", 3]}})"), &problems);
  CHECK_EQ(s.mode, std::string("trusted"));
  CHECK(s.deny.count("mix") == 1 && s.ask.count("shape") == 1 && s.ask.size() == 1);
  CHECK_EQ(problems.size(), (size_t)1);
  problems.clear();
  s = ai::permissionsFrom(json::parse(R"({"permissions": {"mode": "yolo"}})"), &problems);
  CHECK_EQ(s.mode, std::string("ask"));
  CHECK_EQ(problems.size(), (size_t)1);
  CHECK_EQ(ai::permissionsFrom(json::object()).mode, std::string("ask"));
}

void inTheChat(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  auto offered = [](const json& req, const std::string& name) {
    for (const auto& t : req["tools"])
      if (t["name"] == name) return true;
    return false;
  };
  const json shapeCall = json::array({toolUse("a", "shape", {{"shape", "darkpsycore-64"}})});
  const json done = json::array({{{"type", "text"}, {"text", "Feito."}}});

  // Read only: no write tool is offered, the system says so, and a write called anyway is refused.
  {
    ai::Config c;
    c.permissions.mode = "readonly";
    ai::Chat chat(c, fns, &cat);
    json req = chat.ask("explica o drop", song);
    CHECK(offered(req, "ler_musica"));
    CHECK(!offered(req, "pcore") && !offered(req, "shape"));
    CHECK(req["system"][0]["text"].get<std::string>().find("só lês") != std::string::npos);
    req = chat.onResponse(answer(shapeCall, "tool_use"), song);
    CHECK(lastToolResult(req).contains("error"));
    CHECK(chat.proposal()["calls"].empty());
  }
  // Ask (default): the proposal waits for Apply.
  {
    ai::Chat chat(ai::Config{}, fns, &cat);
    chat.ask("forma", song);
    chat.onResponse(answer(shapeCall, "tool_use"), song);
    chat.onResponse(answer(done, "end_turn"), song);
    CHECK_EQ(chat.proposal()["calls"].size(), (size_t)1);
    CHECK(!chat.proposal()["auto"].get<bool>());
  }
  // Trusted: applies by itself; but one call that asks keeps the whole proposal for Apply.
  {
    ai::Config c;
    c.permissions.mode = "trusted";
    ai::Chat chat(c, fns, &cat);
    chat.ask("forma", song);
    chat.onResponse(answer(shapeCall, "tool_use"), song);
    chat.onResponse(answer(done, "end_turn"), song);
    CHECK(chat.proposal()["auto"].get<bool>());
    c.permissions.ask = {"track"};
    ai::Chat careful(c, fns, &cat);
    careful.ask("forma e kick", song);
    careful.onResponse(answer(json::array({toolUse("a", "shape", {{"shape", "darkpsycore-64"}}), toolUse("b", "track", {{"part", "kick"}})}), "tool_use"), song);
    careful.onResponse(answer(done, "end_turn"), song);
    CHECK_EQ(careful.proposal()["calls"].size(), (size_t)2);
    CHECK(!careful.proposal()["auto"].get<bool>());
  }
  // Denied per action: not offered, refused if called, the rest still works.
  {
    ai::Config c;
    c.permissions.deny = {"shape"};
    ai::Chat chat(c, fns, &cat);
    json req = chat.ask("forma", song);
    CHECK(!offered(req, "shape") && offered(req, "pcore"));
    req = chat.onResponse(answer(shapeCall, "tool_use"), song);
    CHECK(lastToolResult(req).contains("error"));
    CHECK(chat.proposal()["calls"].empty());
  }
}

}  // namespace catalog_t

// Lessons of the first AI chat inside Core (core-old b16b2c8, 2026-11-03): a call in a cut-off answer is not run but is
// answered, and a message that failed leaves the conversation.
namespace lessons_t {

using ai_chat_t::answer;
using ai_chat_t::toolUse;

void cutOffCalls(const doc::PCoreCatalog& cat) {
  doc::Document song = doc::newEmptyDocument();
  ai::Chat chat(ai::Config{}, control::buildFunctions(&cat), &cat);
  chat.ask("forma", song);
  CHECK(chat.onResponse(answer(json::array({toolUse("a", "shape", {{"shape", "darkpsycore-64"}})}), "max_tokens"), song).is_null());
  CHECK(chat.proposal()["calls"].empty());  // not run
  json req = chat.ask("outra vez", song);
  const json& msgs = req["messages"];
  // user, assistant (the cut call), user (its answer + the new text): every tool_use has its tool_result.
  CHECK_EQ(msgs.size(), (size_t)3);
  const json& last = msgs.back();
  CHECK_EQ(last["role"].get<std::string>(), std::string("user"));
  CHECK_EQ(last["content"][0]["type"].get<std::string>(), std::string("tool_result"));
  CHECK_EQ(last["content"][0]["tool_use_id"].get<std::string>(), std::string("a"));
  CHECK_EQ(last["content"].back()["type"].get<std::string>(), std::string("text"));
}

void failedMessages(const doc::PCoreCatalog& cat) {
  doc::Document song = doc::newEmptyDocument();
  ai::Chat chat(ai::Config{}, control::buildFunctions(&cat), &cat);
  chat.ask("primeira", song);
  chat.onResponse(answer(json::array({{{"type", "text"}, {"text", "ok"}}}), "end_turn"), song);
  // An API error: the message leaves the conversation.
  chat.ask("apaga tudo", song);
  CHECK(chat.onResponse({{"type", "error"}, {"error", {{"type", "overloaded_error"}, {"message", "busy"}}}}, song).is_null());
  json req = chat.ask("segunda", song);
  CHECK_EQ(req["messages"].size(), (size_t)3);
  CHECK(req["messages"].dump().find("apaga tudo") == std::string::npos);
  // No connection (the app calls abandon): the same, with what the message had proposed.
  chat.onResponse(answer(json::array({toolUse("s", "shape", {{"shape", "darkpsycore-64"}})}), "tool_use"), song);
  CHECK_EQ(chat.proposal()["calls"].size(), (size_t)1);
  chat.abandon();
  CHECK(chat.proposal()["calls"].empty());
  req = chat.ask("terceira", song);
  CHECK_EQ(req["messages"].size(), (size_t)3);
  CHECK(req["messages"].dump().find("segunda") == std::string::npos);
}

}  // namespace lessons_t

// The AI's memory (memory.hpp): recipes of what Diogo applied and liked, like Meow Memory (SQLite, BM25, importance
// that fades unless confirmed), tied to Core's calls so any model can use them.
namespace memory_t {

using ai_chat_t::answer;
using ai_chat_t::lastToolResult;
using ai_chat_t::toolUse;

constexpr int64_t kDay = 86400000;

std::filesystem::path freshDb(const char* name) {
  const auto p = std::filesystem::temp_directory_path() / name;
  for (const char* ext : {"", "-wal", "-shm"}) std::filesystem::remove(p.string() + ext);
  return p;
}
json proposalOf(const std::string& summary, const std::string& part) {
  return {{"summary", summary}, {"calls", json::array({{{"fn", "track"}, {"part", part}}, {{"fn", "pcore"}, {"on", part}, {"part", part}}})}};
}

void recipes() {
  const auto db = freshDb("core-ai-memory-test.sqlite");
  std::string why;
  auto m = ai::Memory::open(db, &why);
  CHECK(m != nullptr);
  if (!m) return;
  const int64_t t0 = 1'800'000'000'000;
  CHECK(m->applied("olá", "", json{{"calls", json::array()}}, "x", {}, t0).empty());  // nothing applied, nothing kept
  const std::string bass = m->applied("faz um baixo KBBB rolante no drop", "darkpsycore", proposalOf("baixo KBBB", "bass"), "claude-sonnet-5-5", {}, t0);
  const std::string kick = m->applied("um kick mais duro", "darkpsycore", proposalOf("kick duro", "kick"), "deepseek-chat", {}, t0);
  const std::string hats = m->applied("hats no drop", "hitech", proposalOf("hats", "hats"), "qwen3:14b", {}, t0);
  CHECK(!bass.empty() && !kick.empty() && bass != kick);
  CHECK_EQ(m->size(), 3);

  // Found by the request's words, accents folded; the calls come back as they were.
  auto hits = m->search("baixo rolante", "darkpsycore", t0);
  CHECK(!hits.empty() && hits[0].id == bass);
  CHECK(!hits.empty() && hits[0].calls.size() == 2 && hits[0].status == "candidate" && hits[0].importance == 1);
  CHECK(m->search("BAIXÓ", "", t0).size() == 1);
  CHECK(m->search("o de a", "", t0).empty());  // only common words: nothing

  // Gostei: confirmed and first; Não gostei: kept but never offered.
  CHECK(m->feedback(kick, true, "assim mesmo", t0 + kDay));
  CHECK(m->feedback(hats, false, "demasiado brilhante", t0 + kDay));
  CHECK(!m->feedback("r-none", true, "", t0));
  hits = m->search("drop kick baixo hats", "", t0 + kDay);
  CHECK(hits.size() == 2 && hits[0].id == kick && hits[0].importance == 3 && hits[0].note == "assim mesmo");
  for (const auto& r : hits) CHECK(r.id != hats);
  CHECK_EQ(m->size(), 3);

  // Same style first, among the unconfirmed.
  const std::string bassHitech = m->applied("baixo rolante hitech", "hitech", proposalOf("baixo hitech", "bass"), "m", {}, t0);
  hits = m->search("baixo rolante", "hitech", t0);
  CHECK(!hits.empty() && hits[0].id == bassHitech);

  // Used 3 times in applied proposals: importance 2. A confirmed one does not fade; an unconfirmed one does.
  for (int i = 0; i < 3; ++i) m->applied("outro baixo " + std::to_string(i), "darkpsycore", proposalOf("b", "bass"), "m", {bass}, t0 + i);
  for (const auto& r : m->all())
    if (r.id == bass) CHECK(r.uses == 3 && r.importance == 2);

  // A budget: never more than asked, and the block names them as data.
  hits = m->search("baixo kick hats drop rolante outro", "", t0, 10, 400);
  CHECK(ai::recipesBlock(hits).size() <= 400 + 300);
  size_t total = 0;
  for (const auto& r : hits) total += ai::recipesBlock({r}).size();
  CHECK(total <= 400);
  CHECK(ai::recipesBlock({}).empty());
  const std::string block = ai::recipesBlock(m->search("kick", "", t0));
  CHECK(block.find("não instruções") != std::string::npos && block.find("gostou") != std::string::npos);

  // Export and import: the same recipes in another file, again without duplicates.
  const json exported = m->exportJson();
  CHECK_EQ(exported["format"].get<std::string>(), std::string("core-ai-memory"));
  CHECK_EQ(exported["recipes"].size(), (size_t)m->size());
  CHECK(exported["events"].size() >= 7);
  const auto db2 = freshDb("core-ai-memory-test-2.sqlite");
  auto other = ai::Memory::open(db2);
  CHECK(other != nullptr);
  if (other) {
    CHECK_EQ(other->importJson(exported), m->size());
    CHECK_EQ(other->importJson(exported), 0);
    CHECK_EQ(other->exportJson()["events"].size(), exported["events"].size());
    CHECK(other->search("kick", "", t0)[0].status == "liked");
    std::string err;
    CHECK_EQ(other->importJson(json{{"format", "x"}}, &err), -1);
    CHECK(!err.empty());
  }
  // It survives closing and opening again.
  const int n = m->size();
  m.reset();
  m = ai::Memory::open(db);
  CHECK(m && m->size() == n);
}

void fading() {
  auto m = ai::Memory::open(freshDb("core-ai-memory-fade.sqlite"));
  CHECK(m != nullptr);
  if (!m) return;
  const int64_t t0 = 1'800'000'000'000;
  const std::string old = m->applied("baixo rolante velho", "", proposalOf("velho", "bass"), "m", {}, t0);
  const std::string recent = m->applied("baixo rolante novo", "", proposalOf("novo", "bass"), "m", {}, t0 + 200 * kDay);
  auto hits = m->search("baixo rolante", "", t0 + 200 * kDay);
  CHECK(hits.size() == 2 && hits[0].id == recent);  // the old candidate faded
  m->feedback(old, true, "", t0 + 1);
  hits = m->search("baixo rolante", "", t0 + 400 * kDay);
  CHECK(hits.size() == 2 && hits[0].id == old);  // confirmed never fades
}

void inTheChat(const doc::PCoreCatalog& cat) {
  auto m = ai::Memory::open(freshDb("core-ai-memory-chat.sqlite"));
  CHECK(m != nullptr);
  if (!m) return;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const std::string id = m->applied("faz um baixo KBBB", "", proposalOf("baixo KBBB", "bass"), "m", {}, now);
  m->feedback(id, true, "", now);
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();

  // The recipe goes with a similar request, and the chat says which it showed.
  ai::Chat chat(ai::Config{}, fns, &cat, nullptr, m.get());
  json req = chat.ask("quero um baixo KBBB", song);
  const std::string sent = req["messages"].back()["content"].get<std::string>();
  CHECK(sent.find("quero um baixo KBBB") == 0);
  CHECK(sent.find("Memória do Core") != std::string::npos);
  CHECK(chat.shown().size() == 1 && chat.shown()[0] == id);
  bool offered = false;
  for (const auto& t : req["tools"]) offered |= t["name"] == "lembrar_receitas";
  CHECK(offered);
  // Nothing alike: the request goes alone.
  req = chat.ask("explica o drop", song);
  CHECK(req["messages"].back()["content"].get<std::string>() == "explica o drop");
  CHECK(chat.shown().empty());
  // The tool finds them too.
  req = chat.onResponse(answer(json::array({toolUse("m", "lembrar_receitas", {{"pergunta", "baixo"}})}), "tool_use"), song);
  CHECK(lastToolResult(req)["recipes"].size() == 1);
  CHECK(chat.shown().size() == 1);

  // Denied: no recipes in the request, no tool.
  ai::Config c;
  c.permissions.deny = {"lembrar_receitas"};
  ai::Chat closed(c, fns, &cat, nullptr, m.get());
  req = closed.ask("quero um baixo KBBB", song);
  CHECK(req["messages"].back()["content"].get<std::string>() == "quero um baixo KBBB");
  for (const auto& t : req["tools"]) CHECK(t["name"] != "lembrar_receitas");
  // The same with another provider: the memory is Core's, not the model's.
  ai::Config o;
  o.provider.kind = "openai";
  ai::Chat other(o, fns, &cat, nullptr, m.get());
  const ai::HttpRequest h = other.http(other.ask("quero um baixo KBBB", song));
  CHECK(h.body.dump().find("Memória do Core") != std::string::npos);
}

// Diogo, 2026-11-16: the memory is exported clean, so AICORE can train a model on it (Core only keeps and exports).
void episodesForTraining(const doc::PCoreCatalog& cat) {
  auto m = ai::Memory::open(freshDb("core-ai-memory-episodes.sqlite"));
  CHECK(m != nullptr);
  if (!m) return;
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  ai::Chat chat(ai::Config{}, fns, &cat, nullptr, m.get());
  chat.ask("cria a estrutura de um darkpsycore", song);
  chat.onResponse(answer(json::array({toolUse("s", "shape", {{"shape", "darkpsycore-64"}})}), "tool_use"), song);
  chat.onResponse(answer(json::array({{{"type", "text"}, {"text", "Pronto."}}}), "end_turn"), song);
  const json e = chat.episode();
  CHECK(e["format"] == "core-ai-episode" && e["version"] == 1 && e["model"] == "claude-haiku-5-5" && e["toolsView"] == "full");
  CHECK(e["system"].get<std::string>().size() > 100);
  CHECK(e["tools"].size() == ai::catalog(fns, false, true).size());
  CHECK(e["messages"].size() == 4 && e["turnStart"] == 0);  // request, tool_use, tool_result, answer
  CHECK(e["log"].size() == 1);
  const json proposal = chat.proposal();
  CHECK(proposal["calls"].size() == 1);

  // Two applied with the same tools: the tools are kept once.
  const std::string a = m->applied("cria a estrutura de um darkpsycore", "darkpsycore", proposal, "claude-haiku-5-5", {}, 1000, e);
  const std::string b = m->applied("outra estrutura", "darkpsycore", proposal, "claude-haiku-5-5", {}, 2000, e);
  m->applied("sem episódio", "", proposal, "m", {}, 3000);
  m->feedback(a, true, "boa estrutura", 1500);
  const json full = m->exportJson();
  CHECK(full["version"] == 2 && full["toolsets"].size() == 1);

  // JSONL: a header, then one whole line per recipe, oldest first.
  const std::string text = m->exportJsonl();
  std::vector<json> lines;
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    lines.push_back(json::parse(text.substr(at, end - at), nullptr, false));
    at = end + 1;
  }
  CHECK(lines.size() == 4);
  if (lines.size() != 4) return;
  CHECK(lines[0]["format"] == "core-ai-memory-jsonl" && lines[0]["recipes"] == 3);
  CHECK(lines[1]["recipe"]["id"] == a && lines[2]["recipe"]["id"] == b);
  CHECK(lines[1]["recipe"]["status"] == "liked" && lines[1]["recipe"]["note"] == "boa estrutura");
  CHECK(lines[1]["episode"]["tools"] == e["tools"] && lines[1]["episode"]["messages"] == e["messages"]);
  CHECK(!lines[1]["episode"].contains("toolsHash") || lines[1]["episode"]["tools"].is_array());
  CHECK(lines[1]["events"].size() == 2 && lines[1]["events"][1]["kind"] == "liked");
  CHECK(!lines[1]["events"][0]["data"].contains("episode"));  // the episode is said once, beside the recipe
  CHECK(lines[3]["episode"].is_null());

  // Import brings the episodes and their tools.
  auto other = ai::Memory::open(freshDb("core-ai-memory-episodes-2.sqlite"));
  CHECK(other && other->importJson(full) == 3);
  if (other) CHECK(other->exportJsonl() == text);
}

}  // namespace memory_t


// Diogo, 2026-11-16: a selection on the tracks (tracks + bars) limits what the AI changes, for build-ups, rolls, drops.
namespace zone_t {

using song_calls_t::named;

doc::Document beat(const doc::PCoreCatalog& cat, const std::vector<control::CoreFunction>& fns) {
  doc::Document song = doc::newEmptyDocument();
  const json raw = {{"calls", json::array({{{"fn", "shape"}, {"shape", "darkpsycore-64"}},
                                            {{"fn", "track"}, {"part", "kick"}, {"name", "Kick"}},
                                            {{"fn", "track"}, {"part", "bass"}, {"name", "Bass"}},
                                            {{"fn", "track"}, {"part", "hats"}, {"name", "Hats"}},
                                            {{"fn", "pcore"}, {"on", "Kick"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}},
                                            {{"fn", "pcore"}, {"on", "Bass"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}},
                                            {{"fn", "pcore"}, {"on", "Hats"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}},
                                            {{"fn", "arrange"}}})}};
  control::ApplyEnv env;
  env.pcore = &cat;
  control::applyProposal(song, control::validateProposal(raw, fns, song, nullptr), "", env);
  return song;
}

// The notes a track plays in the song ticks [from, to): (song tick, pitch), repeats unrolled.
std::vector<std::pair<int64_t, int>> heard(const doc::Track& t, int64_t from, int64_t to) {
  std::vector<std::pair<int64_t, int>> out;
  for (const auto& c : t.clips) {
    const doc::Pattern* p = nullptr;
    for (const auto& x : t.patterns) if (x.id == c.patternId) p = &x;
    if (!p || p->lengthTicks <= 0) continue;
    for (int64_t rep = -p->lengthTicks; rep < c.offsetTicks + c.lengthTicks; rep += p->lengthTicks)
      for (const auto& n : p->notes) {
        const int64_t inClip = rep + n.tick - c.offsetTicks;
        if (inClip < 0 || inClip >= c.lengthTicks || n.tick >= p->lengthTicks) continue;
        const int64_t songTick = c.startTick + inClip;
        if (songTick >= from && songTick < to) out.push_back({songTick, n.pitch});
      }
  }
  std::sort(out.begin(), out.end());
  return out;
}

void rules(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = beat(cat, fns);
  const doc::Track* kick = named(song, "Kick");
  const doc::Track* bass = named(song, "Bass");
  CHECK(kick && bass && song.sections.size() > 3);
  if (!kick || !bass || song.sections.size() < 4) return;
  const int64_t bar = 4 * doc::kPpq;
  const doc::Section& build = song.sections[1];
  control::Zone z;
  z.tracks = {kick->id, bass->id};
  z.startTick = build.startTick;
  z.endTick = build.startTick + 4 * bar;
  const json calls = json::array({
      {{"fn", "pcore"}, {"on", "Kick"}, {"section", "Seleção"}, {"style", "darkpsycore"}},         // the zone's bars
      {{"fn", "pcore"}, {"on", "Hats"}, {"style", "darkpsycore"}},                                   // not selected
      {{"fn", "shape"}, {"shape", "darkpsycore-64"}},                                                // the whole song
      {{"fn", "arrange"}, {"on", "Kick"}},                                                           // no section
      {{"fn", "pcore"}, {"on", "Bass"}, {"section", song.sections.back().name}, {"style", "darkpsycore"}},  // outside
      {{"fn", "pcore"}, {"on", "Bass"}, {"section", build.name}, {"style", "darkpsycore"}, {"args", {{"bars", 16}}}}});  // cut to the zone
  const control::Proposal p = control::validateProposal({{"calls", calls}, {"zone", z.toJson()}}, fns, song, nullptr);
  CHECK(p.zone.active() && p.zone.startTick == z.startTick);
  CHECK_EQ(p.calls.size(), (size_t)2);
  CHECK_EQ(p.problems.size(), (size_t)4);
  for (const auto& pr : p.problems) CHECK(pr.find("left out") != std::string::npos && pr.find("selection") != std::string::npos);
  if (p.calls.size() == 2) {
    CHECK(p.calls[0].startTick == z.startTick && p.calls[0].endTick == z.endTick && p.calls[0].args.at("bars") == 4);
    CHECK(p.calls[0].sectionName == control::kZoneSection && p.calls[0].zoneSection);
    CHECK(p.calls[1].startTick == z.startTick && p.calls[1].endTick == z.endTick && p.calls[1].args.at("bars") == 4);
    CHECK(!p.calls[1].warnings.empty());  // 16 bars cut to 4
  }
  // Without a zone, "Seleção" is not a section.
  CHECK(control::validateProposal({{"calls", json::array({calls[0]})}}, fns, song, nullptr).calls.empty());

  // Applied: the zone's bars change, nothing else does (the pattern there becomes its own).
  const auto kickBefore = heard(*kick, 0, song.lengthTicks), hatsBefore = heard(*named(song, "Hats"), 0, song.lengthTicks);
  const auto outsideBefore = [&] {
    auto a = heard(*kick, 0, z.startTick), b = heard(*kick, z.endTick, song.lengthTicks);
    a.insert(a.end(), b.begin(), b.end());
    return a;
  }();
  const json one = {{"calls", json::array({{{"fn", "pcore"}, {"on", "Kick"}, {"section", "Seleção"}, {"style", "darkpsycore"},
                                             {"shape", "kick/roll-bar"}}})}, {"zone", z.toJson()}};
  const control::Proposal q = control::validateProposal(one, fns, song, nullptr);
  for (const auto& pr : q.problems) std::cerr << "problem: " << pr << "\n";
  control::ApplyEnv env;
  env.pcore = &cat;
  doc::Document after = song;
  if (!q.calls.empty()) control::applyProposal(after, q, "", env);
  const doc::Track* k2 = named(after, "Kick");
  CHECK(k2 != nullptr);
  if (!k2) return;
  auto outsideAfter = heard(*k2, 0, z.startTick);
  const auto tail = heard(*k2, z.endTick, after.lengthTicks);
  outsideAfter.insert(outsideAfter.end(), tail.begin(), tail.end());
  CHECK(outsideAfter == outsideBefore);
  CHECK(heard(*named(after, "Hats"), 0, after.lengthTicks) == hatsBefore);
  CHECK(!heard(*k2, z.startTick, z.endTick).empty());
  CHECK(heard(*k2, z.startTick, z.endTick) != heard(*kick, z.startTick, z.endTick));  // the roll is there now
  int zoneClips = 0;
  for (const auto& c : k2->clips) zoneClips += c.startTick == z.startTick && c.lengthTicks == z.endTick - z.startTick;
  CHECK_EQ(zoneClips, 1);
}

// zonePattern alone: what played there is kept, the edges are cut, a repeating pattern is unrolled.
void zonePattern() {
  doc::Document song = doc::newEmptyDocument();
  doc::Track t;
  t.id = "t1";
  t.name = "Kick";
  doc::Pattern p;
  p.id = "p1";
  p.lengthTicks = 4 * doc::kPpq;  // one bar, repeated in a 4-bar clip
  for (int i = 0; i < 4; ++i) p.notes.push_back({"n" + std::to_string(i), i * doc::kPpq, 240, 36, 0.8});
  t.patterns.push_back(p);
  t.clips.push_back({"c1", "p1", 0, 16 * doc::kPpq, 0});
  const auto before = heard(t, 0, 16 * doc::kPpq);
  const std::string pid = doc::zonePattern(t, 6 * doc::kPpq, 10 * doc::kPpq);  // bar 2, beat 3 .. bar 3, beat 3
  CHECK(heard(t, 0, 16 * doc::kPpq) == before);
  CHECK_EQ(t.clips.size(), (size_t)3);
  const doc::Pattern* z = t.findPattern(pid);
  CHECK(z && z->notes.size() == 4 && z->notes.front().tick == 0 && z->lengthTicks == 4 * doc::kPpq);
}

void inTheChat(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = beat(cat, fns);
  const doc::Track* kick = named(song, "Kick");
  if (!kick) return;
  control::Zone z;
  z.tracks = {kick->id};
  z.startTick = song.sections[1].startTick;
  z.endTick = z.startTick + 8 * doc::kPpq;
  ai::Chat chat(ai::Config{}, fns, &cat);
  json req = chat.ask("faz um roll no fim do build-up", song, &z);
  const std::string sent = req["messages"].back()["content"].get<std::string>();
  CHECK(sent.find("Seleção do Diogo") != std::string::npos && sent.find("Kick") != std::string::npos);
  req = chat.onResponse(ai_chat_t::answer(json::array({ai_chat_t::toolUse("r", "ler_musica", json::object())}), "tool_use"), song);
  const json songRead = ai_chat_t::lastToolResult(req);
  CHECK(songRead["tracks"].size() == 1 && songRead["otherTracks"].size() == 2 && songRead.contains("selection"));
  req = chat.onResponse(ai_chat_t::answer(json::array({ai_chat_t::toolUse("a", "pcore", {{"on", "Kick"}, {"section", "Seleção"}, {"style", "darkpsycore"}}),
                                                       ai_chat_t::toolUse("b", "pcore", {{"on", "Hats"}, {"style", "darkpsycore"}})}),
                                             "tool_use"), song);
  CHECK(chat.proposal()["calls"].size() == 1 && chat.proposal()["zone"]["tracks"][0] == kick->id);
  CHECK(chat.episode()["zone"]["startTick"] == z.startTick);
  // A new message without a selection: the whole song again.
  req = chat.ask("e agora?", song);
  CHECK(!chat.proposal().contains("zone"));
}

}  // namespace zone_t

// The review of 2026-11-16 (C1, C2) and the chat panel's sessions.
namespace review_t {

using ai_chat_t::answer;
using ai_chat_t::toolUse;

// C2: a message stops asking the model once it has cost its limit.
void costLimit(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  ai::Config c;
  c.maxUsdPerMessage = 0.01;
  ai::Chat chat(c, fns, &cat);
  chat.ask("faz uma batida", song);
  // Haiku: 0.10 $/M input. 200k tokens = $0.02, over the limit: no next request.
  json next = chat.onResponse(answer(json::array({toolUse("r", "ler_musica", json::object())}), "tool_use", 200000, 100), song);
  CHECK(next.is_null());
  CHECK(chat.error().find("cost limit") != std::string::npos);
  // Under it, the work goes on.
  ai::Chat cheap(ai::Config{}, fns, &cat);
  cheap.ask("faz uma batida", song);
  next = cheap.onResponse(answer(json::array({toolUse("r", "ler_musica", json::object())}), "tool_use", 2000, 100), song);
  CHECK(!next.is_null() && cheap.error().empty());
}

// C1: Apply after the song changed: a call that no longer fits stops the whole proposal.
void changedSince(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = zone_t::beat(cat, fns);
  const json proposal = {{"calls", json::array({{{"fn", "pcore"}, {"on", "Kick"}, {"style", "darkpsycore"}},
                                                {{"fn", "pcore"}, {"on", "Hats"}, {"style", "darkpsycore"}}})}};
  CHECK(ai::changedSince(proposal, control::validateProposal(proposal, fns, song, nullptr)).empty());
  doc::Document after = song;
  after.tracks.erase(std::remove_if(after.tracks.begin(), after.tracks.end(), [](const doc::Track& t) { return t.name == "Hats"; }), after.tracks.end());
  const std::string why = ai::changedSince(proposal, control::validateProposal(proposal, fns, after, nullptr));
  CHECK(why.find("changed since") != std::string::npos && why.find("nothing was applied") != std::string::npos);
}

// A chat kept on disk is taken up again, with any model; a turn left half way is taken out.
void restore(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  ai::Chat chat(ai::Config{}, fns, &cat);
  chat.ask("olá", song);
  chat.onResponse(answer(json::array({{{"type", "text"}, {"text", "Olá!"}}}), "end_turn"), song);
  const json kept = chat.messages();
  CHECK_EQ(kept.size(), (size_t)2);
  ai::Config o;
  o.provider.kind = "openai";
  ai::Chat again(o, fns, &cat);
  again.restore(kept);
  json req = again.ask("e agora?", song);
  CHECK_EQ(req["messages"].size(), (size_t)3);
  // Half way: a tool call never answered, then a request with no answer: both go.
  json half = kept;
  half.push_back({{"role", "user"}, {"content", "mais"}});
  half.push_back({{"role", "assistant"}, {"content", json::array({toolUse("x", "ler_musica", json::object())})}});
  again.restore(half);
  CHECK(again.messages() == kept);
}

}  // namespace review_t

// Found by the real-API bench (2026-11-16): models write a style's id as "align"; it aligns as that style does.
namespace bench_t {
void alignByStyle(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  const json calls = json::array({{{"fn", "track"}, {"part", "kick"}, {"name", "Kick"}},
                                  {{"fn", "pcore"}, {"on", "Kick"}, {"style", "darkpsycore"}, {"align", "darkpsycore"}},
                                  {{"fn", "pcore"}, {"on", "Kick"}, {"align", "darkpsycore"}},
                                  {{"fn", "pcore"}, {"on", "Kick"}, {"style", "hitech"}, {"align", "darkpsycore"}},
                                  {{"fn", "pcore"}, {"on", "Kick"}, {"align", "nonsense"}}});
  const control::Proposal p = control::validateProposal({{"calls", calls}}, fns, song, nullptr);
  CHECK_EQ(p.calls.size(), (size_t)3);  // the track and the two that name darkpsycore alone
  CHECK_EQ(p.problems.size(), (size_t)2);  // another style's id, and a word that is neither
  if (p.calls.size() == 3) {
    CHECK_EQ(p.calls[1].texts.at("align"), std::string("style"));
    CHECK_EQ(p.calls[2].texts.at("style"), std::string("darkpsycore"));
  }
}
}  // namespace bench_t

// The AI's synth is Vital (Diogo, 2026-11-16): found where its installer puts it, and the default instrument roles.
namespace synth_t {
void vital() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "core-ai-synth-test";
  fs::remove_all(dir);
  fs::create_directories(dir / "a");
  fs::create_directories(dir / "b");
  CHECK(!ai::findSynth({dir / "a", dir / "b"}));
  fs::create_directories(dir / "b" / "Vital.vst3" / "Contents");  // a VST3 bundle is a folder
  const auto found = ai::findSynth({dir / "a", dir / "b"});
  CHECK(found && *found == dir / "b" / "Vital.vst3");
  // The AI's plugins: today the synth, looked for the same way.
  const auto plugins = ai::aiPlugins();
  CHECK(plugins.size() == 1 && plugins[0].file == "Vital.vst3");
  const auto again = ai::findAiPlugin(plugins[0], {dir / "a", dir / "b"});
  CHECK(again && *again == dir / "b" / "Vital.vst3");
  fs::remove_all(dir);
  CHECK(!ai::findAiPlugin(plugins[0], {dir / "b"}));

  const json vital = {{"format", "VST3"}, {"id", "VST3-Vital-1"}, {"name", "Vital"}, {"vendor", "Vital Audio"}, {"file", "C:/x/Vital.vst3"}};
  const json other = {{"format", "VST3"}, {"id", "VST3-Serum-2"}, {"name", "Serum 2"}, {"vendor", "Xfer"}, {"file", "C:/x/Serum2.vst3"}};
  CHECK(ai::findSynthPlugin(json::array({other})).is_null());
  CHECK_EQ(ai::findSynthPlugin(json::array({other, vital}))["id"], json("VST3-Vital-1"));

  // No roles.json yet: every synth role goes to Vital; drums and the effects are not the synth's.
  const json fresh = ai::withSynthRoles(nullptr, json::array({other, vital}));
  for (const char* r : {"bass", "lead", "pad", "pluck"}) CHECK_EQ(fresh["roles"][r]["plugin"]["id"], json("VST3-Vital-1"));
  CHECK(!fresh["roles"].contains("drums"));
  CHECK(!fresh["roles"].contains("reverb"));

  // The user's choice stays: bass on Serum 2 is kept, the empty roles are filled.
  json mine = {{"version", 1}, {"roles", {{"bass", {{"plugin", other}, {"params", json::object()}}}}}};
  const json merged = ai::withSynthRoles(mine, json::array({vital}));
  CHECK_EQ(merged["roles"]["bass"]["plugin"]["id"], json("VST3-Serum-2"));
  CHECK_EQ(merged["roles"]["lead"]["plugin"]["id"], json("VST3-Vital-1"));

  // Without Vital in the scan nothing changes.
  CHECK_EQ(ai::withSynthRoles(mine, json::array({other})), mine);
}
}  // namespace synth_t

// The kinds of calls left out, for the bench (the plan's taxonomy): from Core's own reasons.
namespace taxonomy_t {
void kinds() {
  CHECK_EQ(ai::problemKind("left out: New track part \"build\" is not one of: kick, bass: left out"), std::string("name"));
  CHECK_EQ(ai::problemKind("left out: no section called \"Drop\": P-Core left out (sections: )"), std::string("section"));
  CHECK_EQ(ai::problemKind("left out: Mix volume 3 is outside -60..6: left out"), std::string("range"));
  CHECK_EQ(ai::problemKind("left out: P-Core: no track given (\"on\"): left out"), std::string("missing"));
  CHECK_EQ(ai::problemKind("left out: Mix on the new track Kick: only P-Core, Arrange and Plays can use it in the same proposal: left out"),
           std::string("dependency"));
  CHECK_EQ(ai::problemKind("not allowed: arrange"), std::string("permission"));
  CHECK_EQ(ai::problemKind("left out: something new"), std::string("other"));
}
}  // namespace taxonomy_t

// Streaming (A8): the events become the same message a request without streaming gets.
namespace stream_t {
std::string sse(const json& e) { return "event: " + e.value("type", std::string()) + "\ndata: " + e.dump() + "\n\n"; }

std::string sample() {
  std::string s;
  s += sse({{"type", "message_start"}, {"message", {{"id", "m1"}, {"type", "message"}, {"role", "assistant"}, {"content", json::array()},
                                                   {"stop_reason", nullptr}, {"usage", {{"input_tokens", 10}, {"cache_read_input_tokens", 900}, {"output_tokens", 1}}}}}});
  s += ": ping\n\n";
  s += sse({{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "thinking"}, {"thinking", ""}}}});
  s += sse({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "thinking_delta"}, {"thinking", "a forma primeiro"}}}});
  s += sse({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "signature_delta"}, {"signature", "sig"}}}});
  s += sse({{"type", "content_block_stop"}, {"index", 0}});
  s += sse({{"type", "content_block_start"}, {"index", 1}, {"content_block", {{"type", "text"}, {"text", ""}}}});
  s += sse({{"type", "content_block_delta"}, {"index", 1}, {"delta", {{"type", "text_delta"}, {"text", "Vou criar "}}}});
  s += sse({{"type", "content_block_delta"}, {"index", 1}, {"delta", {{"type", "text_delta"}, {"text", "a forma."}}}});
  s += sse({{"type", "content_block_stop"}, {"index", 1}});
  s += sse({{"type", "content_block_start"}, {"index", 2}, {"content_block", {{"type", "tool_use"}, {"id", "t1"}, {"name", "shape"}, {"input", json::object()}}}});
  s += sse({{"type", "content_block_delta"}, {"index", 2}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"shape\": \"darkps"}}}});
  s += sse({{"type", "content_block_delta"}, {"index", 2}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "ycore-64\"}"}}}});
  s += sse({{"type", "content_block_stop"}, {"index", 2}});
  s += sse({{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}, {"stop_sequence", nullptr}}}, {"usage", {{"output_tokens", 42}}}});
  s += sse({{"type", "message_stop"}});
  return s;
}

void assemble(const doc::PCoreCatalog& cat) {
  const std::string body = sample();
  // Any chunk size gives the same message (the network cuts lines anywhere, \r\n too).
  json first;
  for (size_t chunk : {(size_t)1, (size_t)7, (size_t)64, body.size()}) {
    ai::StreamAssembler a;
    std::string text;
    std::vector<std::string> tools;
    a.onText = [&](const std::string& t) { text += t; };
    a.onTool = [&](const std::string& n) { tools.push_back(n); };
    for (size_t at = 0; at < body.size(); at += chunk) a.feed(std::string_view(body).substr(at, chunk));
    a.finish();
    CHECK(a.sawEvents() && a.complete());
    CHECK_EQ(text, std::string("Vou criar a forma."));
    CHECK(tools.size() == 1 && tools[0] == "shape");
    const json m = a.message();
    if (first.is_null()) first = m;
    CHECK_EQ(m, first);
  }
  CHECK_EQ(first["stop_reason"], json("tool_use"));
  CHECK_EQ(first["usage"]["output_tokens"], json(42));
  CHECK_EQ(first["usage"]["cache_read_input_tokens"], json(900));
  CHECK(first["content"].size() == 3);
  CHECK_EQ(first["content"][0]["thinking"], json("a forma primeiro"));
  CHECK_EQ(first["content"][0]["signature"], json("sig"));
  CHECK_EQ(first["content"][2]["input"], json({{"shape", "darkpsycore-64"}}));

  // The chat takes it as any answer: the call is checked and goes in the proposal.
  const auto fns = control::buildFunctions(&cat);
  ai::Config cfg;
  cfg.stream = true;
  ai::Chat chat(cfg, fns, &cat);
  const doc::Document song = doc::newEmptyDocument();
  const json req = chat.ask("cria a estrutura", song);
  CHECK_EQ(req.value("stream", false), true);
  chat.onResponse(first, song);
  CHECK_EQ(chat.proposal()["calls"].size(), (size_t)1);
  CHECK(chat.usage().output == 42);

  // An OpenAI-compatible provider is not streamed.
  ai::Config o;
  o.stream = true;
  o.provider.kind = "openai";
  ai::Chat other(o, fns, &cat);
  CHECK(!other.ask("olá", song).contains("stream"));
}

void failures() {
  {  // an error event mid-stream
    ai::StreamAssembler a;
    a.feed(sse({{"type", "message_start"}, {"message", {{"content", json::array()}, {"usage", json::object()}}}}));
    a.feed(sse({{"type", "error"}, {"error", {{"type", "overloaded_error"}, {"message", "Overloaded"}}}}));
    a.finish();
    CHECK_EQ(a.message()["error"]["type"], json("overloaded_error"));
  }
  {  // cut before the end: an error, so the message leaves the conversation
    ai::StreamAssembler a;
    const std::string body = sample();
    a.feed(std::string_view(body).substr(0, body.size() / 2));
    a.finish();
    CHECK(!a.complete());
    CHECK_EQ(a.message()["type"], json("error"));
  }
  {  // not a stream at all (an HTTP error's JSON): no events, the app reads the body as JSON
    ai::StreamAssembler a;
    a.feed(R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    a.finish();
    CHECK(!a.sawEvents());
  }
}
}  // namespace stream_t

// The gateway view (ai-daw's help/call idea): "ajuda" + "fazer" in place of every edit tool, the same checks.
namespace gateway_t {
using ai_chat_t::answer;
using ai_chat_t::lastToolResult;
using ai_chat_t::toolUse;

void gatewayMode(const doc::PCoreCatalog& cat) {
  const auto fns = control::buildFunctions(&cat);
  doc::Document song = doc::newEmptyDocument();
  ai::Config c;
  c.gateway = true;
  ai::Chat chat(c, fns, &cat);
  json req = chat.ask("forma e kick", song);
  std::set<std::string> names;
  for (const auto& t : req["tools"]) names.insert(t["name"].get<std::string>());
  CHECK(names.count("ajuda") && names.count("fazer") && names.count("ler_musica"));
  CHECK(!names.count("pcore") && !names.count("shape"));
  CHECK(req["system"][0]["text"].get<std::string>().find("fazer {acao, dados}") != std::string::npos);
  ai::Chat full(ai::Config{}, fns, &cat);
  CHECK(req.dump().size() * 2 < full.ask("forma e kick", song).dump().size() + 2000);  // about half
  // ajuda: the list, then one action's schema (the same as the full view's).
  req = chat.onResponse(answer(json::array({toolUse("h", "ajuda", json::object())}), "tool_use"), song);
  CHECK(lastToolResult(req)["acoes"].size() + 2 == ai::catalog(fns, false).size());  // the two reads are not listed
  req = chat.onResponse(answer(json::array({toolUse("h2", "ajuda", {{"tema", "shape"}})}), "tool_use"), song);
  const json help = lastToolResult(req);
  CHECK(help["acoes"].size() == 1 && help["acoes"][0]["dados"] == ai::findAction(ai::catalog(fns, false), "shape")->schema);
  req = chat.onResponse(answer(json::array({toolUse("h3", "ajuda", {{"tema", "nada"}})}), "tool_use"), song);
  CHECK(lastToolResult(req).contains("error"));
  // fazer: checked and gathered like a direct call; wrong ones are left out with the reason.
  req = chat.onResponse(answer(json::array({toolUse("f1", "fazer", {{"acao", "shape"}, {"dados", {{"shape", "darkpsycore-64"}}}}),
                                            toolUse("f2", "fazer", {{"acao", "track"}, {"dados", {{"part", "kick"}, {"name", "Kick"}}}}),
                                            toolUse("f3", "fazer", {{"acao", "pcore"}, {"dados", {{"on", "Kick"}, {"section", "Nowhere"}}}}),
                                            toolUse("f4", "fazer", {{"acao", "fazer"}, {"dados", json::object()}})}),
                               "tool_use"), song);
  CHECK_EQ(chat.proposal()["calls"].size(), (size_t)2);
  CHECK(lastToolResult(req).contains("error"));
  // Permissions hold inside fazer too.
  ai::Config d = c;
  d.permissions.deny = {"shape"};
  ai::Chat denied(d, fns, &cat);
  req = denied.ask("forma", song);
  for (const auto& t : req["tools"])
    if (t["name"] == "fazer") CHECK(t["input_schema"]["properties"]["acao"]["enum"].dump().find("\"shape\"") == std::string::npos);
  req = denied.onResponse(answer(json::array({toolUse("f", "fazer", {{"acao", "shape"}, {"dados", {{"shape", "darkpsycore-64"}}}})}), "tool_use"), song);
  CHECK(lastToolResult(req).contains("error") && denied.proposal()["calls"].empty());
  // Read only: no edits, so no gateway tools.
  ai::Config r = c;
  r.permissions.mode = "readonly";
  ai::Chat reader(r, fns, &cat);
  for (const auto& t : reader.ask("explica", song)["tools"]) CHECK(t["name"] != "fazer" && t["name"] != "ajuda");
}
}  // namespace gateway_t

TEST("song: a beat from nothing") { song_calls_t::beatFromNothing(catalog()); }
TEST("song: changing a part") { song_calls_t::changeAPart(catalog()); }
TEST("song: what is left out") { song_calls_t::leftOut(catalog()); }
TEST("song: a refused call changes nothing") { song_calls_t::refusedChangesNothing(catalog()); }
TEST("chat: tools") { ai_chat_t::tools(catalog()); }
TEST("chat: a conversation and its cost") { ai_chat_t::conversation(catalog()); }
TEST("chat: errors and limits") { ai_chat_t::errorsAndLimits(catalog()); }
TEST("catalog: one source, every provider's view") { catalog_t::views(catalog()); }
TEST("catalog: allow, deny, ask") { catalog_t::decide(catalog()); }
TEST("catalog: the permissions in the chat") { catalog_t::inTheChat(catalog()); }
TEST("chat: a cut-off call is answered, not run") { lessons_t::cutOffCalls(catalog()); }
TEST("chat: a failed message leaves the conversation") { lessons_t::failedMessages(catalog()); }
TEST("memory: recipes, Gostei, export") { memory_t::recipes(); }
TEST("memory: what is not confirmed fades") { memory_t::fading(); }
TEST("memory: in the chat, with any model") { memory_t::inTheChat(catalog()); }
TEST("zone: the selection limits the proposal") { zone_t::rules(catalog()); }
TEST("zone: its own clip and pattern") { zone_t::zonePattern(); }
TEST("zone: in the chat") { zone_t::inTheChat(catalog()); }
TEST("chat: a cost limit per message") { review_t::costLimit(catalog()); }
TEST("chat: Apply after the song changed") { review_t::changedSince(catalog()); }
TEST("chat: a kept chat taken up again") { review_t::restore(catalog()); }
TEST("memory: episodes and the JSONL export") { memory_t::episodesForTraining(catalog()); }
TEST("song: a style's id as align") { bench_t::alignByStyle(catalog()); }
TEST("catalog: the gateway view (ajuda / fazer)") { gateway_t::gatewayMode(catalog()); }
TEST("ai: the synth is Vital") { synth_t::vital(); }
TEST("ai: the kinds of calls left out") { taxonomy_t::kinds(); }
TEST("ai: streaming, assembled") { stream_t::assemble(catalog()); }
TEST("ai: streaming, when it fails") { stream_t::failures(); }
TEST("ai: models") { ai_knowledge_t::models(); }
TEST("ai: knowledge base") { ai_knowledge_t::knowledgeBase(knowledgeDb()); }
TEST("ai: the consult tool") { ai_knowledge_t::consultTool(knowledgeDb()); }
TEST("ai: OpenAI-compatible provider") { ai_knowledge_t::openAiProvider(); }

CORE_TEST_MAIN
