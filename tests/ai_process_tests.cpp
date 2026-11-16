// O processo da AI de verdade (Fase 4, 2026-11-16): o Core arranca o core-ai e fala com ele por pipes (sem rede).
// O modelo e falso (--fake-model, respostas de um ficheiro) e as AIs "mas" sao guioes (--evil). Cada teste verifica o
// que o Core ve: online / offline e porque, as mensagens que passaram, e que a musica do Core nunca mudou sozinha.
//   P01  hello; uma mensagem inteira com proposta; admitida e aplicada num passo; a receita fica na memoria DO PROCESSO
//   P02  matar o processo a meio de uma proposta: offline, nada aplicado, nenhuma resposta parcial
//   P03  AI que manda meio frame e morre: os bytes sao deitados fora, nada passa
//   P04  AI que manda lixo / um frame gigante / outra mensagem antes do hello: o Core para-a (violacao do protocolo)
//   P05  AI que nunca diz hello: parada ao fim do tempo
//   P06  AI que propoe coisas fora do ambito pela via certa: a mensagem passa, a proposta e recusada
//   P07  sem o executavel; processo que sai sozinho; bye: o Core continua, sem excecoes
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include "ai_admit.hpp"
#include "calls.hpp"
#include "edit.hpp"
#include "follow.hpp"
#include "history.hpp"
#include "ai_link.hpp"
#include "pcore.hpp"
#include "ai_scope.hpp"
#include "support/check.hpp"
#ifndef _WIN32
#include <csignal>
#endif

using namespace core;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const doc::PCoreCatalog& pcore() {
  static const doc::PCoreCatalog c = doc::loadPCoreCatalog(std::string(CORE_SOURCE_DIR) + "/pcore/styles.json");
  return c;
}
const std::vector<control::CoreFunction>& functions() {
  static const std::vector<control::CoreFunction> f = control::buildFunctions(&pcore());
  return f;
}

doc::Document song() {
  doc::Document s = doc::newTemplateDocument();
  doc::Section build, drop;
  build.id = "sec-build";
  build.name = "Build-up";
  build.lengthTicks = 8 * 4 * doc::kPpq;
  drop.id = "sec-drop";
  drop.name = "Drop";
  drop.startTick = 8 * 4 * doc::kPpq;
  drop.lengthTicks = 8 * 4 * doc::kPpq;
  s.sections = {build, drop};
  return s;
}

json hello() {
  json catalog = json::array();
  for (const auto& a : ai::catalog(ai::exposedFunctions(functions()), false)) catalog.push_back(a.name);
  return {{"type", "hello"}, {"protocol", ai::ipc::kProtocol}, {"catalog", catalog}, {"scope", ai::creationScope()}};
}

json ask(const std::string& id, const doc::Document& s) {
  return {{"type", "ask"}, {"id", id}, {"text", "um baixo no drop"}, {"song", doc::toJson(s)}, {"zone", nullptr},
          {"settings", {{"model", ""}, {"effort", 1}, {"mode", "ask"}, {"maxUsd", 1.0}, {"useMemory", true}, {"keepMemory", true}}}};
}

json modelAnswer(const json& content, const std::string& stop) {
  return {{"type", "message"}, {"role", "assistant"}, {"content", content}, {"stop_reason", stop},
          {"usage", {{"input_tokens", 1000}, {"output_tokens", 100}, {"cache_read_input_tokens", 0}, {"cache_creation_input_tokens", 0}}}};
}
json toolUse(const std::string& id, const std::string& name, const json& input) {
  return {{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}};
}
json pcoreTool() { return toolUse("t1", "pcore", {{"on", "Bass"}, {"section", "Drop"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}}); }

// A pasta de cada teste: os dados da AI (memoria, chats, chave) e os guioes.
fs::path folder(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / ("core-ai-process-tests-" + name);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d / "data");
  std::ofstream(d / "data" / "anthropic-key.txt") << "test-key-never-sent";  // o modelo e falso: nada sai daqui
  return d;
}

fs::path write(const fs::path& p, const json& j) {
  std::ofstream(p) << j.dump();
  return p;
}

// O que o Core ve do link: as mensagens que passaram e se ficou offline (e porque).
struct Watch {
  std::mutex m;
  std::condition_variable cv;
  std::vector<json> got;
  std::vector<std::string> logs;
  bool offline = false;
  std::string why;
  void attach(ai::Link& l) {
    l.onMessage = [this](const json& j) { std::lock_guard<std::mutex> g(m); got.push_back(j); cv.notify_all(); };
    l.onOffline = [this](const std::string& w) { std::lock_guard<std::mutex> g(m); offline = true; why = w; cv.notify_all(); };
    l.onLog = [this](const std::string& s) { std::lock_guard<std::mutex> g(m); logs.push_back(s); };
  }
  bool waitFor(const std::string& type, json* out = nullptr, int ms = 10000) {
    std::unique_lock<std::mutex> g(m);
    const bool ok = cv.wait_for(g, std::chrono::milliseconds(ms), [&] {
      for (const auto& j : got)
        if (j.value("type", "") == type) return true;
      return false;
    });
    if (ok && out)
      for (const auto& j : got)
        if (j.value("type", "") == type) *out = j;
    return ok;
  }
  bool waitOffline(int ms = 10000) {
    std::unique_lock<std::mutex> g(m);
    return cv.wait_for(g, std::chrono::milliseconds(ms), [&] { return offline; });
  }
  int count(const std::string& type) {
    std::lock_guard<std::mutex> g(m);
    int n = 0;
    for (const auto& j : got) n += j.value("type", "") == type;
    return n;
  }
  bool logged(const std::string& part) {
    std::lock_guard<std::mutex> g(m);
    for (const auto& s : logs)
      if (s.find(part) != std::string::npos) return true;
    return false;
  }
};

ai::Link::Options real(const fs::path& dir, const fs::path& answers) {
  ai::Link::Options o;
  o.exe = CORE_AI_EXE;
  o.args = {"--data", (dir / "data").string(), "--pcore", std::string(CORE_SOURCE_DIR) + "/pcore/styles.json", "--fake-model", answers.string()};
  return o;
}
ai::Link::Options evil(const fs::path& dir, const json& steps, int helloMs = 8000) {
  ai::Link::Options o;
  o.exe = CORE_AI_EXE;
  o.args = {"--evil", write(dir / "evil.json", steps).string()};
  o.helloMs = helloMs;
  return o;
}

json helloFromAi() { return {{"type", "hello"}, {"protocol", 1}, {"models", {"Fake"}}, {"notices", json::array()}}; }

std::string hex(const std::string& bytes) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) out += {d[c >> 4], d[c & 15]};
  return out;
}

void applyLikeTheApp(doc::History& h, const json& raw) {
  const ai::Admission a = ai::admitProposal(raw, functions(), h.current(), nullptr, {}, {});
  if (!a.ok) throw doc::DocumentError("calls", a.why);
  control::ApplyEnv env;
  env.pcore = &pcore();
  doc::Document next = h.current();
  control::applyProposal(next, a.proposal, "", env);
  doc::refreshFollows(next);
  h.commit(next, control::kAiLabelPrefix + a.proposal.summary);
}

}  // namespace

TEST("processo (P01): hello, uma mensagem com proposta, aplicada num passo; a receita fica na memoria do processo") {
  const fs::path d = folder("p01");
  const json answers = json::array({modelAnswer(json::array({{{"type", "text"}, {"text", "Proponho isto."}}, pcoreTool()}), "tool_use"),
                                    modelAnswer(json::array({{{"type", "text"}, {"text", "Feito."}}}), "end_turn")});
  ai::Link link;
  Watch w;
  w.attach(link);
  std::string why;
  CHECK(link.start(real(d, write(d / "answers.json", answers)), hello(), &why));
  CHECK(w.waitFor("hello"));
  CHECK(link.online());
  doc::History h(song());
  CHECK(link.send(ask("m1", h.current())));
  json answer;
  CHECK(w.waitFor("answer", &answer));
  CHECK_EQ(answer.value("id", ""), std::string("m1"));
  CHECK(answer["proposal"].is_object());
  CHECK(!answer["proposal"].contains("auto"));  // a AI nao decide o que se aplica sozinho
  const std::string before = doc::toJson(h.current()).dump();
  CHECK_EQ(doc::toJson(h.current()).dump(), before);  // a resposta chegou e nada mudou ate ao Apply
  applyLikeTheApp(h, answer["proposal"]);
  doc::History count = h;
  int steps = 0;
  while (count.undo()) ++steps;
  CHECK_EQ(steps, 1);
  // Aplicado: o processo guarda a receita (SQLite dele); Gostei confirma-a.
  CHECK(link.send({{"type", "applied"}, {"id", "m1"}}));
  json recipe;
  CHECK(w.waitFor("recipe", &recipe));
  CHECK(link.send({{"type", "feedback"}, {"recipeId", recipe.value("recipeId", "")}, {"liked", true}}));
  json result;
  CHECK(w.waitFor("result", &result));
  CHECK(result.value("ok", false));
  CHECK(fs::exists(d / "data" / "core-ai-memory.sqlite"));
  link.stop();
  CHECK(!link.online());
  CHECK(fs::exists(d / "data" / "chats"));  // o chat ficou no disco, pelo processo
}

TEST("processo (P02): matar a AI a meio de uma proposta: offline, nada aplicado, nenhuma resposta") {
  const fs::path d = folder("p02");
  // O modelo propoe uma chamada (a proposta fica a meio no processo) e depois nunca mais responde.
  const json answers = json::array({modelAnswer(json::array({pcoreTool()}), "tool_use"), {{"__stall", true}}});
  ai::Link link;
  Watch w;
  w.attach(link);
  CHECK(link.start(real(d, write(d / "answers.json", answers)), hello()));
  CHECK(w.waitFor("hello"));
  doc::History h(song());
  const std::string before = doc::toJson(h.current()).dump();
  CHECK(link.send(ask("m1", h.current())));
  CHECK(w.waitFor("status"));  // a meio: uma chamada ja proposta, o modelo a "pensar"
#ifdef _WIN32
  link.kill("test: the process was killed");
#else
  ::kill((pid_t)link.pid(), SIGKILL);  // de fora, como um crash
#endif
  CHECK(w.waitOffline(5000));
  CHECK(!link.online());
  CHECK_EQ(w.count("answer"), 0);
  CHECK_EQ(doc::toJson(h.current()).dump(), before);
  CHECK(!link.send(ask("m2", h.current())));  // offline: nada se manda, nada rebenta
}

TEST("processo (P03): meio frame e a AI morre: os bytes sao deitados fora, nada passa") {
  const fs::path d = folder("p03");
  const std::string full = ai::ipc::frame({{"type", "answer"}, {"id", "m1"}, {"proposal", {{"calls", {{{"fn", "pcore"}, {"on", "Bass"}}}}}}}, ai::ipc::kMaxFromAi);
  const json steps = json::array({{{"send", helloFromAi()}}, {{"waitFor", "ask"}}, {{"rawHex", hex(full.substr(0, full.size() / 2))}}, {{"exit", 0}}});
  ai::Link link;
  Watch w;
  w.attach(link);
  CHECK(link.start(evil(d, steps), hello()));
  CHECK(w.waitFor("hello"));
  CHECK(link.send(ask("m1", song())));
  CHECK(w.waitOffline());
  CHECK_EQ(w.count("answer"), 0);
  CHECK(w.logged("unfinished message were dropped"));
}

TEST("processo (P04): lixo, frame gigante, mensagem antes do hello: o Core para a AI") {
  struct Case { const char* name; json steps; const char* why; };
  std::string bigHeader = {(char)0x01, (char)0x00, (char)0x10, (char)0x00};  // 1 MiB + 1
  const std::vector<Case> cases = {
      {"lixo", json::array({{{"send", helloFromAi()}}, {{"rawHex", hex(std::string("\x05\x00\x00\x00{nope", 9))}}, {{"stall", true}}}), "protocol"},
      {"gigante", json::array({{{"send", helloFromAi()}}, {{"rawHex", hex(bigHeader)}}, {{"stall", true}}}), "limit"},
      {"antes do hello", json::array({{{"send", {{"type", "status"}, {"id", "x"}, {"text", "hi"}}}}, {{"stall", true}}}), "first message"},
      {"tipo inventado", json::array({{{"send", helloFromAi()}}, {{"send", {{"type", "apply"}, {"proposal", json::object()}}}}, {{"stall", true}}}), "unknown"},
      {"hello duas vezes", json::array({{{"send", helloFromAi()}}, {{"send", helloFromAi()}}, {{"stall", true}}}), "twice"},
  };
  for (const auto& c : cases) {
    const fs::path d = folder(std::string("p04-") + std::to_string(&c - cases.data()));
    ai::Link link;
    Watch w;
    w.attach(link);
    CHECK(link.start(evil(d, c.steps), hello()));
    if (!w.waitOffline(5000)) {
      coretest::fail(__FILE__, __LINE__, std::string(c.name) + ": a AI nao foi parada");
      continue;
    }
    if (w.why.find(c.why) == std::string::npos) coretest::fail(__FILE__, __LINE__, std::string(c.name) + ": porque '" + w.why + "'");
    CHECK(w.logged("protocol violation"));
  }
}

TEST("processo (P05): a AI que nunca diz hello e parada ao fim do tempo") {
  const fs::path d = folder("p05");
  ai::Link link;
  Watch w;
  w.attach(link);
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(link.start(evil(d, json::array({{{"stall", true}}}), 400), hello()));
  CHECK(w.waitOffline(5000));
  CHECK(w.why.find("hello") != std::string::npos);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
}

TEST("processo (P06): propostas fora do ambito pela via certa: a mensagem passa, a proposta e recusada") {
  const fs::path d = folder("p06");
  const std::vector<json> proposals = {
      {{"id", "x"}, {"calls", {{{"fn", "mix"}, {"on", "Bass"}, {"args", {{"gainDb", 12}}}}}}},
      {{"id", "x"}, {"calls", {{{"fn", "pcore"}, {"on", "Nobody"}, {"section", "Drop"}, {"style", "darkpsycore"}}}}},
      {{"id", "x"}, {"calls", {{{"fn", "pcore"}, {"on", "Bass"}, {"section", "Drop"}, {"style", "darkpsycore"}, {"args", {{"bars", -5}}}}}}},
  };
  json steps = json::array({{{"send", helloFromAi()}}, {{"waitFor", "ask"}}});
  for (const auto& p : proposals) steps.push_back({{"send", {{"type", "answer"}, {"id", "m1"}, {"proposal", p}}}});
  steps.push_back({{"stall", true}});
  ai::Link link;
  Watch w;
  w.attach(link);
  CHECK(link.start(evil(d, steps), hello()));
  CHECK(w.waitFor("hello"));
  doc::History h(song());
  const std::string before = doc::toJson(h.current()).dump();
  CHECK(link.send(ask("m1", h.current())));
  for (int i = 0; i < 50 && w.count("answer") < (int)proposals.size(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(w.count("answer"), (int)proposals.size());
  std::lock_guard<std::mutex> g(w.m);
  for (const auto& a : w.got) {
    if (a.value("type", "") != "answer") continue;
    CHECK_THROWS(applyLikeTheApp(h, a["proposal"]));
  }
  CHECK_EQ(doc::toJson(h.current()).dump(), before);
  CHECK(link.online());  // uma proposta recusada nao e violacao: a AI continua
}

TEST("processo (P07): sem executavel, saida sozinha e bye: o Core continua") {
  {
    ai::Link link;
    Watch w;
    w.attach(link);
    ai::Link::Options o;
    o.exe = (fs::temp_directory_path() / "no-such-core-ai-here").string();
    std::string why;
    const bool started = link.start(o, hello(), &why);
    // posix_spawn pode criar o processo e so depois falhar o exec: das duas maneiras o Core fica offline.
    if (started) CHECK(w.waitOffline(5000));
    else CHECK(!why.empty());
    CHECK(!link.online());
    CHECK(!link.send({{"type", "bye"}}));
  }
  {
    const fs::path d = folder("p07");
    ai::Link link;
    Watch w;
    w.attach(link);
    CHECK(link.start(evil(d, json::array({{{"send", helloFromAi()}}, {{"exit", 3}}})), hello()));
    CHECK(w.waitOffline());
    CHECK(w.why.find("code 3") != std::string::npos);
  }
  {
    const fs::path d = folder("p07b");
    const json answers = json::array();
    ai::Link link;
    Watch w;
    w.attach(link);
    CHECK(link.start(real(d, write(d / "answers.json", answers)), hello()));
    CHECK(w.waitFor("hello"));
    const auto t0 = std::chrono::steady_clock::now();
    link.stop(3000);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2500));  // saiu pelo bye, sem ser morto
    CHECK(w.offline);
    CHECK_EQ(w.why, std::string("stopped"));
  }
}

CORE_TEST_MAIN
