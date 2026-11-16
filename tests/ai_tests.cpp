// The AI inside Core (2026-11-16): the song's calls (app/control/song_calls), the chat with its tools and costs
// (app/ai/ai_chat), the local knowledge base, the providers and the models (app/ai). Canned answers, no network.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include "ai_chat.hpp"
#include "calls.hpp"
#include "catalog.hpp"
#include "edit.hpp"
#include "knowledge.hpp"
#include "models.hpp"
#include "pcore.hpp"
#include "provider.hpp"
#include "score.hpp"
#include "support/check.hpp"

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
  CHECK_EQ(req["model"].get<std::string>(), std::string("claude-sonnet-5-5"));
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
TEST("ai: models") { ai_knowledge_t::models(); }
TEST("ai: knowledge base") { ai_knowledge_t::knowledgeBase(knowledgeDb()); }
TEST("ai: the consult tool") { ai_knowledge_t::consultTool(knowledgeDb()); }
TEST("ai: OpenAI-compatible provider") { ai_knowledge_t::openAiProvider(); }

CORE_TEST_MAIN
