// A fronteira entre o Core e a AI (Fase 4, 2026-11-16: a AI e um plugin, num processo seu). Sem processo e sem rede:
//   L01-L03  o protocolo: frames, mensagens partidas, limites de tamanho e de profundidade, cada campo de cada mensagem
//   L04      o ambito fisico: a lista fechada de acoes de criacao musical (nada entra por omissao)
//   L05      propostas adversariais: recusadas inteiras, com o porque, sem mudar o documento nem a historia
//   L06      a zona do Core ganha sempre a da AI
//   L07      a mesma proposta aplicada duas vezes: o mesmo resultado, um Undo cada
//   L08-L09  fuzz ao protocolo e as propostas (semente fixa): nunca rebenta, nunca admite o que esta fora
#include <random>
#include <set>
#include "ai_admit.hpp"
#include "calls.hpp"
#include "catalog.hpp"
#include "edit.hpp"
#include "follow.hpp"
#include "history.hpp"
#include "pcore.hpp"
#include "ai_protocol.hpp"
#include "ai_scope.hpp"
#include "support/check.hpp"

using namespace core;
using json = nlohmann::json;
namespace ipc = core::ai::ipc;

namespace {

const doc::PCoreCatalog& pcore() {
  static const doc::PCoreCatalog c = doc::loadPCoreCatalog(std::string(CORE_SOURCE_DIR) + "/pcore/styles.json");
  return c;
}
const std::vector<control::CoreFunction>& functions() {
  static const std::vector<control::CoreFunction> f = control::buildFunctions(&pcore());
  return f;
}

const doc::Document& fixture() {
  static const doc::Document d = [] {
    doc::Document s = doc::newTemplateDocument();
    s.name = "Fronteira";
    doc::Section build, drop;
    build.id = "sec-build";
    build.name = "Build-up";
    build.startTick = 0;
    build.lengthTicks = 8 * 4 * doc::kPpq;
    drop.id = "sec-drop";
    drop.name = "Drop";
    drop.startTick = 8 * 4 * doc::kPpq;
    drop.lengthTicks = 8 * 4 * doc::kPpq;
    s.sections = {build, drop};
    return s;
  }();
  return d;
}

std::string idOf(const doc::Document& d, const std::string& name) {
  for (const auto& t : d.tracks)
    if (t.name == name) return t.id;
  return "?";
}

json goodCall() { return {{"fn", "pcore"}, {"on", "Bass"}, {"section", "Drop"}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}}; }
json goodProposal() { return {{"id", "ai-1"}, {"summary", "Baixo no drop"}, {"calls", json::array({goodCall()})}}; }

ai::Admission admit(const json& raw, const doc::Document& song = fixture(), const ai::Permissions& p = {}, const control::Zone& z = {}) {
  return ai::admitProposal(raw, functions(), song, nullptr, p, z);
}

// Como a app aplica (MainComponent::applyAiProposal): admitir, provar numa copia, e so entao um passo de Undo.
// Recusada: lanca, e a historia nao muda.
void applyLikeTheApp(doc::History& h, const json& raw, const control::Zone& zone = {}) {
  const ai::Admission a = ai::admitProposal(raw, functions(), h.current(), nullptr, {}, zone);
  if (!a.ok) throw doc::DocumentError("calls", a.why);
  control::ApplyEnv env;
  env.pcore = &pcore();
  doc::Document probe = h.current();  // recusada aqui, antes de qualquer passo
  control::applyProposal(probe, a.proposal, "", env);
  doc::Document next = h.current();
  control::applyProposal(next, a.proposal, "", env);
  doc::refreshFollows(next);
  h.commit(next, control::kAiLabelPrefix + a.proposal.summary);
}

int undoSteps(doc::History h) {
  int n = 0;
  while (h.undo()) ++n;
  return n;
}

std::string bytes(const doc::Document& d) { return doc::toJson(d).dump(); }

}  // namespace

// ------------------------------------------------------------------------------------------------------------ L01

TEST("protocolo (L01): um frame vai e volta; partido byte a byte; dois frames no mesmo bloco") {
  const json m = {{"type", "status"}, {"id", "a1"}, {"text", "Ola \xc3\xa7"}};
  const std::string f = ipc::frame(m, ipc::kMaxFromAi);
  CHECK(f.size() > 4);
  ipc::Decoder d(ipc::kMaxFromAi);
  std::vector<json> got;
  for (char c : f)
    for (auto& x : d.feed(&c, 1)) got.push_back(x);
  CHECK_EQ(got.size(), (size_t)1);
  CHECK(got[0] == m);
  CHECK(!d.failed());
  const std::string two = f + ipc::frame({{"type", "error"}, {"text", "x"}}, ipc::kMaxFromAi);
  ipc::Decoder d2(ipc::kMaxFromAi);
  CHECK_EQ(d2.feed(two.data(), two.size()).size(), (size_t)2);
  CHECK_EQ(d2.pending(), (size_t)0);
  // Maior que o limite: nao se manda.
  CHECK(ipc::frame({{"type", "status"}, {"id", "a"}, {"text", std::string(2000, 'x')}}, 100).empty());
}

TEST("protocolo (L02): frames partidos param o leitor, com o porque, e ele fica parado") {
  auto failsWith = [](const std::string& bytes, const std::string& part) {
    ipc::Decoder d(1024);
    const auto out = d.feed(bytes.data(), bytes.size());
    CHECK(out.empty());
    CHECK(d.failed());
    if (d.error().find(part) == std::string::npos) coretest::fail(__FILE__, __LINE__, "erro '" + d.error() + "' sem '" + part + "'");
    // Depois do erro nada mais passa, nem um frame valido.
    const std::string ok = ipc::frame({{"type", "error"}, {"text", "x"}}, 1024);
    CHECK(d.feed(ok.data(), ok.size()).empty());
  };
  auto raw = [](uint32_t n, const std::string& payload) {
    std::string s{(char)(n & 0xFF), (char)((n >> 8) & 0xFF), (char)((n >> 16) & 0xFF), (char)((n >> 24) & 0xFF)};
    return s + payload;
  };
  failsWith(raw(0, ""), "empty");
  failsWith(raw(5000, ""), "limit");                            // so o cabecalho: recusado antes de esperar 5000 bytes
  failsWith(raw(0xFFFFFFFFu, ""), "limit");
  failsWith(raw(5, "{nope"), "brackets");
  failsWith(raw(6, "{\"a\":}"), "JSON");
  failsWith(raw(13, "{\"type\":\"\xff\xfe\"}"), "JSON");        // UTF-8 invalido
  failsWith(raw(2, "[]"), "object");
  failsWith(raw(7, "{\"a\":1}"), "type");
  failsWith(raw(12, "{\"type\":1.5}"), "type");
  // Profundidade: 64 passa, 65 nao (antes de se fazer o parse).
  auto nested = [](int depth) {
    std::string s = "{\"type\":\"status\",\"x\":";
    for (int i = 1; i < depth; ++i) s += "[";
    s += "0";
    for (int i = 1; i < depth; ++i) s += "]";
    return s + "}";
  };
  CHECK_EQ(ipc::depthOf(nested(64)), 64);
  std::string why;
  CHECK(!ipc::parseMessage(nested(64), &why).is_null());
  CHECK(ipc::parseMessage(nested(65), &why).is_null());
  CHECK(why.find("deeper") != std::string::npos);
  CHECK(ipc::parseMessage(std::string(100000, '[') + std::string(100000, ']'), &why).is_null());  // sem descer a pilha
  CHECK_EQ(ipc::depthOf("{\"s\":\"{{{[[[\"}"), 1);  // parenteses dentro de texto nao contam
}

TEST("protocolo (L03): cada mensagem da AI verificada campo a campo; tipo desconhecido recusado") {
  const std::vector<json> good = {
      {{"type", "hello"}, {"protocol", 1}, {"models", {"Claude Haiku"}}, {"notices", json::array()}},
      {{"type", "status"}, {"id", "a"}, {"text", "Working"}},
      {{"type", "partial"}, {"id", "a"}, {"text", "ola"}},
      {{"type", "answer"}, {"id", "a"}, {"reply", "ok"}, {"log", json::array()}, {"proposal", nullptr}, {"cost", {{"message", 0.001}, {"chat", 0.002}, {"requests", 2}}}},
      {{"type", "answer"}, {"id", "a"}, {"proposal", goodProposal()}},
      {{"type", "recipe"}, {"id", "a"}, {"recipeId", "r-1"}},
      {{"type", "result"}, {"op", "count"}, {"ok", true}, {"data", {{"recipes", 3}}}},
      {{"type", "error"}, {"text", "x"}},
  };
  for (const auto& m : good) {
    const std::string why = ipc::checkFromAi(m);
    if (!why.empty()) coretest::fail(__FILE__, __LINE__, "recusou uma mensagem boa: " + why);
  }
  const std::vector<json> bad = {
      {{"type", "hello"}, {"protocol", 2}},                                   // outra versao
      {{"type", "status"}, {"id", std::string(65, 'a')}, {"text", "x"}},       // id comprido
      {{"type", "status"}, {"id", "a"}, {"text", std::string(5000, 'x')}},     // texto comprido
      {{"type", "status"}, {"id", 3}, {"text", "x"}},                          // id que nao e texto
      {{"type", "answer"}, {"id", "a"}, {"proposal", "drop the bass"}},        // proposta que nao e objeto
      {{"type", "answer"}, {"id", "a"}, {"log", {1, 2}}},
      {{"type", "answer"}, {"id", "a"}, {"cost", {{"message", -1}}}},
      {{"type", "answer"}, {"id", "a"}, {"log", json(std::vector<std::string>(129, "x"))}},
      {{"type", "recipe"}, {"id", "a"}},
      {{"type", "result"}, {"op", "count"}, {"ok", "yes"}},
      {{"type", "apply"}, {"proposal", goodProposal()}},                       // a AI nunca aplica
      {{"type", "shell"}, {"cmd", "del"}},
      {{"type", "ask"}, {"id", "a"}, {"text", "x"}, {"song", json::object()}}, // mensagem do Core, nao da AI
  };
  for (const auto& m : bad) {
    if (ipc::checkFromAi(m).empty()) coretest::fail(__FILE__, __LINE__, "aceitou: " + m.dump().substr(0, 120));
  }
  // O lado da AI tambem verifica o que o Core manda.
  CHECK(ipc::checkToAi({{"type", "ask"}, {"id", "a"}, {"text", "x"}, {"song", json::object()}}).empty());
  CHECK(!ipc::checkToAi({{"type", "ask"}, {"id", "a"}, {"text", "x"}}).empty());
  CHECK(!ipc::checkToAi({{"type", "answer"}}).empty());
}

// ------------------------------------------------------------------------------------------------------------ L04

TEST("ambito (L04): a lista fechada e so criacao musical; cada nome existe no Core; nada entra por omissao") {
  const std::set<std::string> expected{"pcore", "shape", "arrange", "plays", "track", "roll", "stutter", "voicestutter"};
  CHECK(std::set<std::string>(ai::creationScope().begin(), ai::creationScope().end()) == expected);
  std::set<std::string> all;
  for (const auto& f : functions()) all.insert(f.id);
  for (const auto& s : ai::creationScope())
    if (!all.count(s)) coretest::fail(__FILE__, __LINE__, "o ambito nomeia uma funcao que o Core nao tem: " + s);
  const auto exposed = ai::exposedFunctions(functions());
  CHECK_EQ(exposed.size(), expected.size());
  for (const auto& f : exposed) CHECK(expected.count(f.id) == 1);
  // Mistura, efeitos, sound design e janelas ficam de fora.
  for (const char* out : {"mix", "open", "gate", "space", "fx", "suck", "pump", "buildup"}) CHECK(!ai::inCreationScope(out));
  // Uma funcao nova do Core fica fora ate uma decisao a por na lista.
  std::vector<control::CoreFunction> withNew = functions();
  control::CoreFunction extra;
  extra.id = "masterize";
  extra.family = "mix";
  withNew.push_back(extra);
  CHECK_EQ(ai::exposedFunctions(withNew).size(), expected.size());
  // O catalogo que o Core expoe a AI so tem essas acoes de escrita.
  for (const auto& a : ai::catalog(exposed, false))
    if (a.kind == ai::Kind::edit) CHECK(expected.count(a.function) == 1);
}

// ------------------------------------------------------------------------------------------------------------ L05

TEST("adversarial (L05): propostas mas sao recusadas inteiras, com o porque; a historia nao muda") {
  CHECK(admit(goodProposal()).ok);
  struct Case { const char* name; json raw; const char* why; };
  json oneGoodOneBad = goodProposal();
  oneGoodOneBad["calls"].push_back({{"fn", "pcore"}, {"on", "Nao existe"}, {"section", "Drop"}, {"style", "darkpsycore"}});
  json many = goodProposal();
  for (int i = 0; i < 64; ++i) many["calls"].push_back(goodCall());
  json bigSummary = goodProposal();
  bigSummary["summary"] = std::string(100000, 'x');
  json longId = goodProposal();
  longId["id"] = std::string(65, 'i');
  const std::vector<Case> cases = {
      {"nao e objeto", json::array({goodCall()}), "not an object"},
      {"sem calls", {{"id", "x"}}, "no list"},
      {"calls vazias", {{"id", "x"}, {"calls", json::array()}}, "no calls"},
      {"65 calls", many, "more than"},
      {"call sem fn", {{"calls", {{{"on", "Bass"}}}}}, "no \"fn\""},
      {"fn numerica", {{"calls", {{{"fn", 7}}}}}, "no \"fn\""},
      {"mistura (mix)", {{"calls", {{{"fn", "mix"}, {"on", "Bass"}, {"args", {{"gainDb", -6}}}}}}}, "not something the AI may do"},
      {"efeito (fx)", {{"calls", {{{"fn", "fx"}, {"on", "Bass"}, {"intent", "buildup"}, {"section", "Build-up"}}}}}, "not something"},
      {"gate", {{"calls", {{{"fn", "gate"}, {"on", "Bass"}, {"section", "Drop"}, {"preset", "psy-bass-gap"}}}}}, "not something"},
      {"janela (open)", {{"calls", {{{"fn", "open"}, {"on", "Bass"}, {"open", "effects"}}}}}, "not something"},
      {"funcao inventada", {{"calls", {{{"fn", "rm -rf"}}}}}, "not something"},
      {"pista desconhecida", {{"calls", {{{"fn", "pcore"}, {"on", "Ghost"}, {"section", "Drop"}, {"style", "darkpsycore"}}}}}, ""},
      {"seccao desconhecida", {{"calls", {{{"fn", "pcore"}, {"on", "Bass"}, {"section", "Outro 9"}, {"style", "darkpsycore"}}}}}, ""},
      {"estilo desconhecido", {{"calls", {{{"fn", "pcore"}, {"on", "Bass"}, {"section", "Drop"}, {"style", "polka"}}}}}, ""},
      {"valor fora de gama", {{"calls", {{{"fn", "pcore"}, {"on", "Bass"}, {"section", "Drop"}, {"style", "darkpsycore"}, {"args", {{"bars", 1e12}}}}}}}, ""},
      {"forma inventada", {{"calls", {{{"fn", "shape"}, {"shape", "nope-999"}}}}}, ""},
      {"uma boa e uma ma", oneGoodOneBad, ""},
      {"resumo gigante", bigSummary, "summary"},
      {"id comprido", longId, "id"},
  };
  for (const auto& c : cases) {
    doc::History h(fixture());
    const std::string before = bytes(h.current());
    const ai::Admission a = admit(c.raw);
    if (a.ok) {
      coretest::fail(__FILE__, __LINE__, std::string("admitida: ") + c.name);
      continue;
    }
    CHECK(!a.why.empty());
    if (std::string(c.why).size() && a.why.find(c.why) == std::string::npos)
      coretest::fail(__FILE__, __LINE__, std::string(c.name) + ": porque '" + a.why + "' sem '" + c.why + "'");
    CHECK_THROWS(applyLikeTheApp(h, c.raw));
    CHECK_EQ(bytes(h.current()), before);
    CHECK_EQ(undoSteps(h), 0);
  }
  // Permissoes do Core: so leitura recusa qualquer escrita; uma acao negada tambem.
  ai::Permissions ro;
  ro.mode = "readonly";
  CHECK(!admit(goodProposal(), fixture(), ro).ok);
  ai::Permissions deny;
  deny.deny = {"pcore"};
  const ai::Admission d = admit(goodProposal(), fixture(), deny);
  CHECK(!d.ok);
  CHECK(d.why.find("denied") != std::string::npos);
  // "auto" da AI nunca chega a proposta admitida: quem decide aplicar sozinho e o Core.
  json withAuto = goodProposal();
  withAuto["auto"] = true;
  const ai::Admission w = admit(withAuto);
  CHECK(w.ok);
  CHECK(!w.raw.contains("auto"));
}

// ------------------------------------------------------------------------------------------------------------ L06

TEST("zona (L06): a zona que o Core mandou substitui a da proposta; fora dela e recusada") {
  const doc::Document& s = fixture();
  control::Zone core;
  core.tracks = {idOf(s, "Bass")};
  core.startTick = 8 * 4 * doc::kPpq;
  core.endTick = 12 * 4 * doc::kPpq;
  json widened = goodProposal();
  widened["zone"] = {{"tracks", json::array({idOf(s, "Bass"), idOf(s, "Kick"), idOf(s, "Hats")})}, {"startTick", 0}, {"endTick", 16 * 4 * doc::kPpq}};
  const ai::Admission a = admit(widened, s, {}, core);
  CHECK(a.ok);
  CHECK(a.raw["zone"] == core.toJson());
  // Uma chamada noutra pista: fora da zona do Core, recusada (mesmo que a "zona" da AI a incluisse).
  json other = widened;
  other["calls"][0]["on"] = "Hats";
  other["calls"][0]["style"] = "darkpsycore";
  const ai::Admission b = admit(other, s, {}, core);
  CHECK(!b.ok);
  // Sem zona no Core: a da AI e tirada (a musica toda, como o Diogo pediu).
  const ai::Admission c = admit(widened, s, {}, control::Zone{});
  CHECK(c.ok);
  CHECK(!c.raw.contains("zone"));
}

// ------------------------------------------------------------------------------------------------------------ L07

TEST("idempotencia (L07): a mesma proposta duas vezes da o mesmo resultado, um Undo cada") {
  // Uma aplicacao = um passo de Undo; aplicar outra vez sobre o resultado nao muda nada (a historia nao ganha um
  // passo vazio: History::commit); do mesmo estado de partida, o resultado e igual byte a byte.
  json p = goodProposal();
  p["calls"].push_back({{"fn", "shape"}, {"shape", "darkpsycore-64"}});
  // Do mesmo estado, duas vezes: byte a byte igual.
  doc::History a(fixture()), b(fixture());
  applyLikeTheApp(a, p);
  applyLikeTheApp(b, p);
  CHECK_EQ(bytes(a.current()), bytes(b.current()));
  CHECK_EQ(undoSteps(a), 1);
  // Duas vezes seguidas: o mesmo resultado, e o Undo volta exatamente ao de antes.
  doc::History h(fixture());
  const std::string s0 = bytes(h.current());
  applyLikeTheApp(h, p);
  const std::string s1 = bytes(h.current());
  applyLikeTheApp(h, p);
  CHECK_EQ(bytes(h.current()), s1);
  CHECK_EQ(undoSteps(h), 1);
  h.undo();
  CHECK_EQ(bytes(h.current()), s0);
  // Undo e aplicar de novo: o mesmo s1.
  applyLikeTheApp(h, p);
  CHECK_EQ(bytes(h.current()), s1);
  // Uma proposta que acrescenta (uma pista nova): duas vezes = duas pistas, um Undo cada, e cada Undo exato.
  const json add = {{"id", "ai-2"}, {"summary", "Lead"}, {"calls", {{{"fn", "track"}, {"part", "lead"}, {"name", "Lead"}}}}};
  doc::History t(fixture());
  const std::string t0 = bytes(t.current());
  applyLikeTheApp(t, add);
  const std::string t1 = bytes(t.current());
  applyLikeTheApp(t, add);
  CHECK_EQ(t.current().tracks.size(), fixture().tracks.size() + 2);
  CHECK_EQ(undoSteps(t), 2);
  t.undo();
  CHECK_EQ(bytes(t.current()), t1);
  t.undo();
  CHECK_EQ(bytes(t.current()), t0);
}

// ------------------------------------------------------------------------------------------------------------ L08

TEST("fuzz (L08): bytes ao calhas e frames mutados nunca rebentam; nada de fora do ambito e admitido") {
  std::mt19937 rng(20261116);
  // Bytes ao calhas, em blocos ao calhas.
  for (int i = 0; i < 3000; ++i) {
    ipc::Decoder d(4096);
    std::string bytes((size_t)(rng() % 200), '\0');
    for (auto& c : bytes) c = (char)(rng() & 0xFF);
    size_t at = 0;
    while (at < bytes.size()) {
      const size_t n = std::min(bytes.size() - at, (size_t)(1 + rng() % 17));
      for (const auto& m : d.feed(bytes.data() + at, n)) (void)ipc::checkFromAi(m);
      at += n;
    }
  }
  // Frames validos de uma resposta com proposta, com bytes trocados, cortados ou acrescentados.
  const json answer = {{"type", "answer"}, {"id", "a1"}, {"reply", "toma"}, {"proposal", goodProposal()}};
  const std::string base = ipc::frame(answer, ipc::kMaxFromAi);
  int admitted = 0, refused = 0;
  for (int i = 0; i < 4000; ++i) {
    std::string f = base;
    const int kind = (int)(rng() % 3);
    const int edits = 1 + (int)(rng() % 4);
    for (int e = 0; e < edits; ++e) {
      if (f.empty()) break;
      const size_t at = rng() % f.size();
      if (kind == 0) f[at] = (char)(rng() & 0xFF);
      else if (kind == 1) f.resize(at);
      else f.insert(at, 1, (char)(rng() & 0xFF));
    }
    ipc::Decoder d(ipc::kMaxFromAi);
    for (const auto& m : d.feed(f.data(), f.size())) {
      if (!ipc::checkFromAi(m).empty() || m.value("type", "") != "answer" || !m.contains("proposal") || !m["proposal"].is_object()) continue;
      const ai::Admission a = admit(m["proposal"]);
      if (!a.ok) { ++refused; continue; }
      ++admitted;
      for (const auto& c : a.raw["calls"]) CHECK(ai::inCreationScope(c["fn"].get<std::string>()));
      doc::History h(fixture());
      applyLikeTheApp(h, m["proposal"]);
      CHECK_EQ(undoSteps(h), 1);
    }
  }
  CHECK(refused > 0);  // o fuzz chegou mesmo a propostas mutadas
  std::printf("  fuzz L08: %d admitidas (intactas na parte que conta), %d recusadas\n", admitted, refused);
}

TEST("fuzz (L09): campos das chamadas trocados por valores ao calhas; admit nunca lanca") {
  std::mt19937 rng(161126);
  auto randomValue = [&](int depth, auto& self) -> json {
    switch (rng() % (depth > 3 ? 6 : 8)) {
      case 0: return nullptr;
      case 1: return (bool)(rng() & 1);
      case 2: return (int64_t)rng() - (int64_t)2147483648LL;
      case 3: return std::ldexp((double)rng(), (int)(rng() % 200) - 100);
      case 4: return std::string((size_t)(rng() % 300), (char)('a' + rng() % 26));
      case 5: { const char* names[] = {"Bass", "Kick", "Drop", "Build-up", "darkpsycore", "master", "Seleção", "", "pcore"}; return names[rng() % 9]; }
      case 6: { json a = json::array(); for (int i = 0, n = (int)(rng() % 4); i < n; ++i) a.push_back(self(depth + 1, self)); return a; }
      default: { json o = json::object(); for (int i = 0, n = (int)(rng() % 4); i < n; ++i) o[std::to_string(rng() % 5)] = self(depth + 1, self); return o; }
    }
  };
  const std::vector<std::string> keys = {"fn", "on", "section", "style", "args", "shape", "part", "name", "play", "preset", "tracks", "from"};
  int admitted = 0;
  for (int i = 0; i < 3000; ++i) {
    json p = goodProposal();
    json& c = p["calls"][0];
    const int edits = 1 + (int)(rng() % 3);
    for (int e = 0; e < edits; ++e) c[keys[rng() % keys.size()]] = randomValue(0, randomValue);
    if (rng() % 4 == 0) c["fn"] = ai::creationScope()[rng() % ai::creationScope().size()];
    ai::Admission a;
    try {
      a = admit(p);
    } catch (const std::exception& e) {
      coretest::fail(__FILE__, __LINE__, std::string("admit lancou: ") + e.what() + " em " + p.dump().substr(0, 200));
      continue;
    }
    if (!a.ok) continue;
    ++admitted;
    CHECK(ai::inCreationScope(a.raw["calls"][0]["fn"].get<std::string>()));
    doc::History h(fixture());
    try {
      applyLikeTheApp(h, p);
      CHECK_EQ(undoSteps(h), 1);
    } catch (const std::exception&) {
      CHECK_EQ(undoSteps(h), 0);  // recusada ao aplicar: nada mudou
    }
  }
  std::printf("  fuzz L09: %d de 3000 admitidas\n", admitted);
}

CORE_TEST_MAIN
