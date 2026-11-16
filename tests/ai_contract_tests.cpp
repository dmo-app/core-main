// Rede de seguranca da AI (plano de testes, Fase 1: T05-T08). Sem modelo e sem rede.
//
//   T05  um modelo falso deterministico: respostas fixas, a conversa toda passa pelo Chat como com o Claude.
//   T06  um contrato por acao de escrita, e um meta-teste: o catalogo (ai::catalog) e a lista dos contratos tem de bater.
//        Acao nova sem contrato, ou contrato tirado, deixa o teste vermelho com o nome da acao.
//   T07  undo unico: aplicar como a app (MainComponent::applyAiProposal + edit) da UM passo; Undo volta ao documento
//        de antes (igual byte a byte no toJson), Redo ao de depois.
//   T08  ambito: o que muda no documento fica dentro do que a acao declara (pista, seccoes...); fora disso, vermelho
//        com o caminho do campo.
// Cada verificador tem um caso negativo (uma acao partida de proposito) que mostra que apanha o erro.
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include "ai_chat.hpp"
#include "assets.hpp"
#include "calls.hpp"
#include "catalog.hpp"
#include "edit.hpp"
#include "follow.hpp"
#include "history.hpp"
#include "pcore.hpp"
#include "roles.hpp"
#include "stutter.hpp"
#include "synth_roles.hpp"
#include "support/check.hpp"
#include "core/project.hpp"
#include "io/wav.hpp"

using namespace core;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const doc::PCoreCatalog& pcore() {
  static const doc::PCoreCatalog c = doc::loadPCoreCatalog(std::string(CORE_SOURCE_DIR) + "/pcore/styles.json");
  return c;
}

// As funcoes do Core, uma vez: um Call guarda um ponteiro para a sua funcao, por isso a lista tem de viver tanto
// como as propostas validadas com ela.
const std::vector<control::CoreFunction>& functions() {
  static const std::vector<control::CoreFunction> f = control::buildFunctions(&pcore());
  return f;
}

json plugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", "VST3-" + name}, {"name", name}, {"vendor", "Test"}, {"file", "C:/VST3/" + name + ".vst3"}};
}

// Os plugins dos papeis (como em aifx_tests), mais o Core AI Gate.
const doc::RoleMap& roles() {
  static const doc::RoleMap m = [] {
    doc::RoleMap r = doc::roleMapFromJson(ai::withAiRoles(json::object(), json::array({plugin("Core AI Gate")})));
    r.roles["reverb"] = doc::guessRoleBinding("reverb", plugin("Room"), json::array({{{"id", "0"}, {"name", "Mix"}}, {{"id", "2"}, {"name", "Decay"}}}));
    r.roles["delay"] = doc::guessRoleBinding("delay", plugin("Delay"), json::array({{{"id", "3"}, {"name", "Dry/Wet"}}, {{"id", "4"}, {"name", "Feedback"}}}));
    r.roles["filter"] = doc::guessRoleBinding("filter", plugin("Filter"), json::array({{{"id", "10"}, {"name", "Cutoff"}}, {{"id", "11"}, {"name", "Resonance"}}}));
    r.roles["drive"] = doc::guessRoleBinding("drive", plugin("Drive"), json::array({{{"id", "5"}, {"name", "Drive"}}, {{"id", "6"}, {"name", "Mix"}}}));
    r.roles["sidechain"] = doc::guessRoleBinding("sidechain", plugin("Shaper"), json::array({{{"id", "7"}, {"name", "Volume"}}}));
    return r;
  }();
  return m;
}

control::ApplyEnv env() {
  control::ApplyEnv e;
  e.pcore = &pcore();
  e.roles = &roles();
  return e;
}

// A musica de partida de todos os contratos: o template (Kick, Bass, Hats...), as seccoes Build-up e Drop, e uma
// pista de audio "Vox" com uma palavra (um WAV gerado) no inicio.
const doc::Document& fixture() {
  static const doc::Document d = [] {
    doc::Document s = doc::newTemplateDocument();
    s.name = "Contratos";
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
    // Notas do Bass com uma seminima: o STUTTER so repete notas com espaco para as repeticoes.
    for (auto& t : s.tracks)
      if (t.name == "Bass")
        for (auto& pt : t.patterns)
          for (auto& n : pt.notes) n.length = std::min<int64_t>(doc::kPpq, pt.lengthTicks - n.tick);
    const fs::path dir = fs::temp_directory_path() / "core-ai-contract-tests";
    fs::create_directories(dir);
    std::vector<float> word(48000 / 2);
    for (size_t i = 0; i < word.size(); ++i) word[i] = 0.3f * (float)std::sin(2 * 3.14159265358979 * 220 * (double)i / 48000);
    writeWav((dir / "vox.wav").string(), word, word, 48000, 24);
    const std::string asset = doc::importAudio(s, dir / "vox.wav", dir);
    const std::string vox = doc::addAudioTrack(s, "Vox");
    doc::addAudioClip(s, vox, asset, 0);
    (void)core::fromJson(doc::toProjection(s));  // a fixture e uma musica que o motor aceita
    return s;
  }();
  return d;
}

std::string idOf(const doc::Document& d, const std::string& name) {
  for (const auto& t : d.tracks)
    if (t.name == name) return t.id;
  return "?" + name;
}

// --- T08: o que mudou, como caminhos. As listas de objetos com "id" (pistas, clips, padroes, efeitos...) sao
// indexadas pelo id, para que acrescentar uma pista nao "mude" as que vem depois: tracks[bass]/mix/gainDb,
// tracks[lead]#added, tracks[hats]#removed, tracks#order.
void diffPaths(const json& a, const json& b, const std::string& at, std::set<std::string>& out) {
  if (a.type() != b.type()) { out.insert(at); return; }
  if (a.is_object()) {
    std::set<std::string> keys;
    for (auto it = a.begin(); it != a.end(); ++it) keys.insert(it.key());
    for (auto it = b.begin(); it != b.end(); ++it) keys.insert(it.key());
    for (const auto& k : keys) {
      const std::string p = at.empty() ? k : at + "/" + k;
      if (!a.contains(k) || !b.contains(k)) out.insert(p);
      else diffPaths(a[k], b[k], p, out);
    }
    return;
  }
  if (a.is_array()) {
    auto byId = [](const json& arr) {
      std::map<std::string, const json*> m;
      for (const auto& x : arr) {
        if (!x.is_object() || !x.contains("id") || !x["id"].is_string()) return std::map<std::string, const json*>{};
        m[x["id"].get<std::string>()] = &x;
      }
      return m;
    };
    const auto ma = byId(a), mb = byId(b);
    if ((!ma.empty() || a.empty()) && (!mb.empty() || b.empty()) && !(a.empty() && b.empty())) {
      std::set<std::string> ids;
      for (const auto& [k, v] : ma) ids.insert(k);
      for (const auto& [k, v] : mb) ids.insert(k);
      for (const auto& id : ids) {
        const std::string p = at + "[" + id + "]";
        if (!ma.count(id)) out.insert(p + "#added");
        else if (!mb.count(id)) out.insert(p + "#removed");
        else diffPaths(*ma.at(id), *mb.at(id), p, out);
      }
      // a ordem tambem conta
      std::vector<std::string> oa, ob;
      for (const auto& x : a) if (mb.count(x["id"])) oa.push_back(x["id"]);
      for (const auto& x : b) if (ma.count(x["id"])) ob.push_back(x["id"]);
      if (oa != ob) out.insert(at + "#order");
      return;
    }
    if (a != b) out.insert(at);
    return;
  }
  if (a != b) out.insert(at);
}

std::set<std::string> changedPaths(const doc::Document& before, const doc::Document& after) {
  std::set<std::string> out;
  diffPaths(doc::toJson(before), doc::toJson(after), "", out);
  return out;
}

// Os caminhos fora do ambito (cada entrada do ambito e uma regex que tem de casar com o inicio do caminho).
std::vector<std::string> outOfScope(const std::set<std::string>& changed, const std::vector<std::string>& scope) {
  std::vector<std::string> out;
  for (const auto& p : changed) {
    bool ok = false;
    for (const auto& s : scope)
      if (std::regex_search(p, std::regex("^(" + s + ")"))) { ok = true; break; }
    if (!ok) out.push_back(p);
  }
  return out;
}

// --- T07: aplicar como a app. MainComponent::applyAiProposal valida, aplica numa copia (recusa antes de qualquer
// passo) e chama edit(), que aplica outra vez, corre refreshFollows, confirma que o motor aceita a projecao e faz UM
// commit. Este e o mesmo caminho, sem JUCE.
using Applier = std::function<void(doc::History&, const control::Proposal&)>;
void applyLikeTheApp(doc::History& h, const control::Proposal& p) {
  doc::Document probe = h.current();
  const auto lines = control::applyProposal(probe, p, "", env());
  doc::Document next = h.current();
  control::applyProposal(next, p, "", env());
  doc::refreshFollows(next);
  (void)core::fromJson(doc::toProjection(next));
  h.commit(next, control::kAiLabelPrefix + (p.summary.empty() ? lines.front() : p.summary));
}

struct UndoReport {
  int steps = 0;            // passos de undo que a aplicacao deixou
  bool undoIsBefore = false, redoIsAfter = false;
  std::set<std::string> changed;
  std::string error;
};

UndoReport undoCheck(const doc::Document& start, const control::Proposal& p, const Applier& apply) {
  UndoReport r;
  doc::History h(start);
  const std::string before = doc::toJson(start).dump();
  try {
    apply(h, p);
  } catch (const std::exception& e) {
    r.error = e.what();
    return r;
  }
  const doc::Document after = h.current();
  r.changed = changedPaths(start, after);
  doc::History count = h;
  while (count.undo()) ++r.steps;
  h.undo();
  r.undoIsBefore = doc::toJson(h.current()).dump() == before;
  h.redo();
  r.redoIsAfter = doc::toJson(h.current()).dump() == doc::toJson(after).dump();
  return r;
}

// --- T06: os contratos. Um por acao de escrita do catalogo. "call" e a chamada como o modelo a faz (os nomes das
// pistas sao os da fixture); "scope" o que a acao pode mudar; "changes": se tem de mudar alguma coisa.
struct Contract {
  json call;
  std::vector<std::string> scope;
  bool changes = true;
};

std::string T(const std::string& name) { return "tracks\\[" + idOf(fixture(), name) + "\\]"; }

std::map<std::string, Contract> contracts() {
  const std::string build = "Build-up", drop = "Drop";
  std::map<std::string, Contract> c;
  c["stutter"] = {{{"fn", "stutter"}, {"on", "Bass"}, {"section", drop}, {"preset", doc::stutterPresets().front().id}},
                  {T("Bass") + "/patterns", T("Bass") + "/clips"}};
  c["voicestutter"] = {{{"fn", "voicestutter"}, {"on", "Vox"}, {"section", build}, {"preset", doc::voiceStutterPresets().front().id}},
                       {T("Vox") + "/audioClips"}};
  c["mix"] = {{{"fn", "mix"}, {"on", "Bass"}, {"args", {{"gainDb", -7.5}}}}, {T("Bass") + "/mix/gainDb"}};
  c["open"] = {{{"fn", "open"}, {"on", "Bass"}, {"open", "effects"}}, {}, false};  // abrir uma janela nao muda a musica
  c["track"] = {{{"fn", "track"}, {"part", "lead"}, {"name", "Lead"}}, {"tracks\\[[^\\]]+\\]#added$"}};
  c["pcore"] = {{{"fn", "pcore"}, {"on", "Bass"}, {"section", drop}, {"style", "darkpsycore"}, {"args", {{"bars", 4}}}},
                {T("Bass") + "/patterns", T("Bass") + "/clips"}};
  c["shape"] = {{{"fn", "shape"}, {"shape", "darkpsycore-64"}}, {"sections", "lengthTicks", "transport/loop"}};
  c["arrange"] = {{{"fn", "arrange"}}, {"tracks\\[[^\\]]+\\]/clips", "tracks\\[[^\\]]+\\]/patterns"}};
  c["plays"] = {{{"fn", "plays"}, {"on", "Hats"}, {"section", drop}, {"play", "off"}}, {T("Hats") + "/clips", T("Hats") + "/patterns"}};
  c["gate"] = {{{"fn", "gate"}, {"on", "Bass"}, {"section", drop}, {"preset", "psy-bass-gap"}},
               {T("Bass") + "/effects", T("Bass") + "/automation"}};
  c["space"] = {{{"fn", "space"}, {"tracks", json::array({"Bass", "Hats"})}, {"kind", "reverb"}, {"preset", "hall"}},
                {"tracks\\[[^\\]]+\\]#added$", T("Bass") + "/mix/sends", T("Hats") + "/mix/sends"}};  // o bus novo e os sends
  c["fx"] = {{{"fn", "fx"}, {"on", "Bass"}, {"intent", "buildup"}, {"section", build}, {"args", {{"amount", 0.5}}}},
             {T("Bass") + "/effects", T("Bass") + "/automation"}};
  c["roll"] = {{{"fn", "roll"}, {"on", "Kick"}, {"section", build}, {"style", "gradual"}}, {T("Kick") + "/patterns", T("Kick") + "/clips"}};
  c["suck"] = {{{"fn", "suck"}, {"on", "Bass"}, {"section", build}}, {T("Bass") + "/effects", T("Bass") + "/automation"}};
  c["pump"] = {{{"fn", "pump"}, {"on", "Bass"}, {"from", "Kick"}, {"section", drop}, {"args", {{"depth", 0.6}}}},
               {T("Bass") + "/effects", T("Bass") + "/automation"}};
  c["buildup"] = {{{"fn", "buildup"}, {"on", "Bass"}, {"section", build}}, {T("Bass") + "/effects", T("Bass") + "/automation"}};
  return c;
}

// As acoes de escrita do catalogo inteiro (com base de conhecimento, memoria e audicao: tudo o que existe).
std::vector<std::string> editActions() {
  std::vector<std::string> out;
  for (const auto& a : ai::catalog(functions(), true, true, true))
    if (a.kind == ai::Kind::edit) out.push_back(a.name);
  return out;
}

// O meta-teste: acoes sem contrato, e contratos de acoes que ja nao existem.
std::vector<std::string> coverageProblems(const std::vector<std::string>& actions, const std::map<std::string, Contract>& cs) {
  std::vector<std::string> out;
  for (const auto& a : actions)
    if (!cs.count(a)) out.push_back("acao sem contrato: " + a);
  for (const auto& [name, c] : cs)
    if (std::find(actions.begin(), actions.end(), name) == actions.end()) out.push_back("contrato de uma acao que nao existe: " + name);
  return out;
}

control::Proposal proposalOf(const json& call, const doc::Document& song) {
  return control::validateProposal({{"id", "contrato"}, {"summary", call.value("fn", std::string())}, {"calls", json::array({call})}},
                                   functions(), song, nullptr);
}

// --- T05: o modelo falso. Responde a cada pedido com a resposta seguinte do guiao, no formato da Anthropic, e conta
// os pedidos. Nao ha rede no Chat (ai_chat.hpp); aqui nem ha "app" que os mande: o teste corre sem chave nem modelo.
class FakeModel {
public:
  explicit FakeModel(std::vector<json> script) : script_(std::move(script)) {}
  json respond(const json& request) {
    requests_.push_back(request);
    if (next_ >= script_.size()) return answer(json::array({{{"type", "text"}, {"text", "(o guiao acabou)"}}}), "end_turn");
    return script_[next_++];
  }
  const std::vector<json>& requests() const { return requests_; }
  static json answer(const json& content, const std::string& stop) {
    return {{"type", "message"}, {"role", "assistant"}, {"content", content}, {"stop_reason", stop},
            {"usage", {{"input_tokens", 1000}, {"output_tokens", 100}, {"cache_read_input_tokens", 0}, {"cache_creation_input_tokens", 0}}}};
  }
  static json toolUse(const std::string& id, const std::string& name, const json& input) {
    return {{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}};
  }

private:
  std::vector<json> script_;
  size_t next_ = 0;
  std::vector<json> requests_;
};

// Uma mensagem inteira: ask, e cada resposta do modelo ate o Chat dizer que acabou.
json runMessage(ai::Chat& chat, FakeModel& model, const std::string& text, const doc::Document& song) {
  json body = chat.ask(text, song);
  int guard = 0;
  while (!body.is_null() && guard++ < 20) body = chat.onResponse(model.respond(body), song);
  return chat.proposal();
}

// O guiao: o modelo pede todas as acoes dos contratos de uma vez (uma chamada por acao, cada uma no seu tool_use).
std::vector<json> scriptAllContracts() {
  json content = json::array({{{"type", "text"}, {"text", "Proponho isto."}}});
  int i = 0;
  for (const auto& [name, c] : contracts()) {
    json input = c.call;
    input.erase("fn");
    content.push_back(FakeModel::toolUse("t" + std::to_string(i++), name, input));
  }
  return {FakeModel::answer(content, "tool_use"), FakeModel::answer(json::array({{{"type", "text"}, {"text", "Feito."}}}), "end_turn")};
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- T06

TEST("contratos (T06): cada acao de escrita do catalogo tem contrato, e cada contrato uma acao") {
  const auto actions = editActions();
  CHECK(actions.size() >= 16);
  const auto problems = coverageProblems(actions, contracts());
  for (const auto& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
  CHECK(problems.empty());
}

TEST("contratos (T06), caso negativo: tirar um contrato ou acrescentar uma acao deixa o meta-teste vermelho, com o nome") {
  auto cs = contracts();
  cs.erase("pump");
  auto problems = coverageProblems(editActions(), cs);
  CHECK_EQ(problems.size(), (size_t)1);
  CHECK(!problems.empty() && problems[0] == "acao sem contrato: pump");
  auto actions = editActions();
  actions.push_back("acao_nova");
  problems = coverageProblems(actions, contracts());
  CHECK_EQ(problems.size(), (size_t)1);
  CHECK(!problems.empty() && problems[0] == "acao sem contrato: acao_nova");
}

// ---------------------------------------------------------------------------------------------------------- T07 + T08

TEST("contratos (T07, T08): cada acao e aceite, da um so passo de undo, Undo/Redo exatos, e so muda o seu ambito") {
  for (const auto& [name, c] : contracts()) {
    const control::Proposal p = proposalOf(c.call, fixture());
    if (!p.problems.empty() || p.calls.size() != 1) {
      CHECK_EQ(name + ": " + (p.problems.empty() ? std::string("sem chamada") : p.problems.front()), name + ": aceite");
      continue;
    }
    const UndoReport r = undoCheck(fixture(), p, applyLikeTheApp);
    if (!r.error.empty()) {
      CHECK_EQ(name + ": " + r.error, name + ": aplicada");
      continue;
    }
    if (!c.changes) {
      CHECK_EQ(name + ": " + std::to_string(r.changed.size()) + " campos mudados", name + ": 0 campos mudados");
      CHECK_EQ(r.steps, 0);
      continue;
    }
    CHECK_EQ(name + ": " + (r.changed.empty() ? "nada mudou" : "mudou"), name + ": mudou");
    CHECK_EQ(name + ": " + std::to_string(r.steps) + " passos de undo", name + ": 1 passos de undo");
    CHECK_EQ(name + ": undo " + (r.undoIsBefore ? "exato" : "diferente"), name + ": undo exato");
    CHECK_EQ(name + ": redo " + (r.redoIsAfter ? "exato" : "diferente"), name + ": redo exato");
    if (std::getenv("CORE_CONTRACT_PATHS"))
      for (const auto& path : r.changed) std::fprintf(stderr, "  %s: %s\n", name.c_str(), path.c_str());
    for (const auto& path : outOfScope(r.changed, c.scope)) CHECK_EQ(name + " mudou fora do ambito: " + path, name + ": dentro do ambito");
  }
}

TEST("undo (T07), caso negativo: uma acao que faz dois commits e apanhada") {
  const Contract c = contracts().at("mix");
  const control::Proposal p = proposalOf(c.call, fixture());
  const Applier twice = [](doc::History& h, const control::Proposal& prop) {
    doc::Document a = h.current();
    a.name = "passo partido";
    h.commit(a, "primeiro");  // de proposito: um passo a mais
    doc::Document b = h.current();
    control::applyProposal(b, prop, "", env());
    h.commit(b, "segundo");
  };
  const UndoReport r = undoCheck(fixture(), p, twice);
  CHECK_EQ(r.steps, 2);
  CHECK(!r.undoIsBefore);  // um Undo so deixa a musica a meio
}

TEST("ambito (T08), caso negativo: uma acao que muda outra pista e apanhada, com o caminho do campo") {
  const Contract c = contracts().at("mix");
  const control::Proposal p = proposalOf(c.call, fixture());
  const Applier leaks = [](doc::History& h, const control::Proposal& prop) {
    doc::Document next = h.current();
    control::applyProposal(next, prop, "", env());
    for (auto& t : next.tracks)
      if (t.name == "Kick") t.mix.pan = 0.5;  // de proposito: fora do ambito
    h.commit(next, "mix partido");
  };
  const UndoReport r = undoCheck(fixture(), p, leaks);
  const auto out = outOfScope(r.changed, c.scope);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK(!out.empty() && out[0] == "tracks[" + idOf(fixture(), "Kick") + "]/mix/pan");
}

// ---------------------------------------------------------------------------------------------------------------- T05

TEST("modelo falso (T05): uma mensagem inteira sem modelo nem rede; a proposta aplica-se num passo de undo") {
  ai::Config cfg;
  cfg.permissions.mode = "ask";
  ai::Chat chat(cfg, functions(), &pcore());
  FakeModel model(scriptAllContracts());
  const json proposal = runMessage(chat, model, "faz tudo o que sabes", fixture());
  CHECK_EQ(model.requests().size(), (size_t)2);  // as chamadas, depois o "feito"
  for (const auto& rq : model.requests()) CHECK(rq.contains("messages") && rq.contains("tools") && rq.value("model", std::string()) == cfg.model);
  CHECK(chat.error().empty());
  CHECK_EQ(chat.reply(), std::string("Feito."));
  const size_t want = contracts().size();
  CHECK_EQ(proposal["calls"].size(), want);
  for (const auto& l : chat.log())
    if (l.find("left out") != std::string::npos) std::fprintf(stderr, "  %s\n", l.c_str());

  const control::Proposal p = control::validateProposal(proposal, functions(), fixture(), nullptr);
  CHECK(p.problems.empty());
  const UndoReport r = undoCheck(fixture(), p, applyLikeTheApp);
  CHECK(r.error.empty());
  CHECK_EQ(r.steps, 1);
  CHECK(r.undoIsBefore && r.redoIsAfter);
}

TEST("modelo falso (T05/T09): a mesma mensagem duas vezes da a mesma proposta e a mesma musica") {
  auto once = [] {
    ai::Chat chat(ai::Config{}, functions(), &pcore());
    FakeModel model(scriptAllContracts());
    json proposal = runMessage(chat, model, "faz tudo o que sabes", fixture());
    proposal.erase("id");
    doc::History h(fixture());
    applyLikeTheApp(h, control::validateProposal(proposal, functions(), fixture(), nullptr));
    return std::make_pair(proposal.dump(), doc::toJson(h.current()).dump());
  };
  const auto a = once(), b = once();
  CHECK(a.first == b.first);
  CHECK(a.second == b.second);
}

CORE_TEST_MAIN
