#include "ai_chat.hpp"
#include "kick_bass.hpp"
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <chrono>
#include "samples.hpp"
#include "score.hpp"

namespace core::ai {

namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* kSystem =
    "És a AI do Core, a DAW do Diogo, para música electrónica (psytrance, dark psycore, hi-tech, slambient...). "
    "Ajudas a fazer batidas, a estrutura das músicas e a mudar partes. Falas português de Portugal, curto e claro.\n\n"
    "Como trabalhas:\n"
    "- Não mudas a música directamente. Cada ferramenta de escrita é uma chamada do Core que fica numa proposta; "
    "o Diogo vê-a e carrega em Aplicar, e tudo entra como um só passo de undo.\n"
    "- Cada chamada é verificada pelo Core logo que a fazes: a resposta diz \"ok\" ou porque ficou de fora. Se ficou "
    "de fora, corrige e volta a tentar, ou explica.\n"
    "- Lê a música (ler_musica) antes de mexer numa música que já tem pistas ou secções.\n"
    "- Para uma batida: pistas que faltem (track), depois P-Core (pcore) em cada pista, com o mesmo estilo e compassos "
    "para kick, baixo e hats, para ficarem alinhados. Se a música ainda não tem secções e o pedido fala de uma (o drop...), "
    "começa por shape. \"A estrutura\" de uma música é completa: shape, as pistas que faltem (track: kick, bass, hats e as "
    "outras partes do estilo), pcore em cada uma e por fim arrange; não pares na forma. Para mudar uma parte: "
    "plays (tirar ou pôr uma pista numa secção) e pcore com mode alter ou outra forma.\n"
    "- Usa os ids exactos das opções (formas, estilos, escalas). Para ver as formas de uma parte, usa formas_pcore.\n"
    "- Quando não souberes como se faz algo num estilo (BPM, KBBB, energia, arranjo, mistura), consulta o manual "
    "(consultar_manual) com poucas palavras-chave, antes de propor.\n"
    "- Uma pista nova não tem som até o Diogo escolher o plugin (VST3); diz-lhe isso quando criares pistas.\n"
    "- Para cortar ou bombear uma pista no tempo (trance gate, o bass a deixar espaço ao kick), usa gate com um preset; "
    "antes de cortar o bass, ouve o kick e o bass (ler_kick_bass) se a ferramenta existir.\n"
    "- Escreve pouco: entre chamadas não escrevas texto (as chamadas ficam registadas na proposta) e não repitas o que "
    "as chamadas já dizem.\n"
    "- No fim, responde com uma frase curta (até 25 palavras): o que propuseste e o que ele deve ouvir. Esse texto é o "
    "título da proposta.";

// The song as the model reads it: Core's song (control::songJson) plus, per track, its part, its pattern and where it
// plays.
json songForAi(const doc::Document& song) {
  json s = control::songJson(song, nullptr);
  for (auto& tj : s["tracks"]) {
    const doc::Track* t = song.findTrack(tj.value("id", std::string()));
    if (!t) continue;
    const std::string part = doc::scorePartOf(*t);
    if (!part.empty()) tj["part"] = part;
    json patterns = json::array();
    for (const auto& p : t->patterns)
      patterns.push_back({{"name", p.name}, {"bars", (double)p.lengthTicks / (4.0 * doc::kPpq)}, {"notes", p.notes.size()}});
    if (!patterns.empty()) tj["patterns"] = patterns;
    json plays = json::array();
    for (const auto& sec : song.sections)
      if (doc::scorePlays(*t, sec)) plays.push_back(sec.name);
    if (!song.sections.empty()) tj["playsIn"] = plays;
  }
  return s;
}

json styleCard(const doc::Document& song, const doc::PCoreCatalog* pcore) {
  json card = json::object();
  json shapes = json::array();
  for (const auto& sh : doc::scoreShapes()) {
    json st = json::array();
    for (const auto& x : sh.stages) st.push_back({x.name, x.bars});
    shapes.push_back({{"id", sh.id}, {"style", sh.style}, {"sections", st}, {"about", sh.info}});
  }
  card["songShapes"] = shapes;
  card["arrangeParts"] = doc::scoreParts();
  if (!pcore) return card;
  const doc::PCoreStyle* style = doc::pcoreStyleFor(*pcore, doc::songStyle(song));
  if (!style) style = doc::pcoreStyleFor(*pcore, song.name);
  json styles = json::array();
  for (const auto& s : pcore->styles) styles.push_back({{"id", s.id}, {"name", s.name}, {"bpm", s.bpm}, {"root", s.root}});
  card["styles"] = styles;
  json parts = json::array();
  for (const auto& p : pcore->parts) parts.push_back({{"id", p.id}, {"group", p.group}, {"about", p.about}});
  card["parts"] = parts;
  if (style) {
    json favourite = json::object();
    for (const auto& [part, ids] : style->shapes) favourite[part] = ids;
    card["songStyle"] = {{"id", style->id}, {"name", style->name}, {"bpm", style->bpm}, {"root", style->root}, {"scales", style->scales},
                         {"about", style->about}, {"shapesByPart", favourite}};
  }
  return card;
}

json shapesFor(const doc::PCoreCatalog* pcore, const std::string& part, const std::string& style) {
  if (!pcore) return {{"error", "the P-Core catalogue is not loaded"}};
  json out = json::array();
  for (const auto* s : doc::pcoreShapesFor(*pcore, part, style))
    out.push_back({{"id", s->id}, {"name", s->name}, {"kind", s->kind}, {"grid", s->grid}, {"steps", s->steps}, {"about", s->about}});
  if (out.empty()) return {{"error", "no shapes for the part \"" + part + "\""}};
  return {{"part", part}, {"shapes", out}};
}

}  // namespace

Prices pricesOf(const std::string& model) {
  if (model.find("haiku") != std::string::npos) return {0.10, 0.50, 0.125, 0.01};
  if (model.find("opus") != std::string::npos) return {4.00, 20.00, 5.00, 0.20};
  return {2.00, 10.00, 2.50, 0.10};
}

void Usage::add(const json& u) {
  if (!u.is_object()) return;
  auto get = [&](const char* k) { return u.contains(k) && u[k].is_number_integer() ? u[k].get<int64_t>() : 0; };
  input += get("input_tokens");
  output += get("output_tokens");
  cacheWrite += get("cache_creation_input_tokens");
  cacheRead += get("cache_read_input_tokens");
  ++requests;
}

double Usage::costUsd(const std::string& model) const { return costUsd(pricesOf(model)); }

double Usage::costUsd(const Prices& p) const {
  return (input * p.input + output * p.output + cacheWrite * p.cacheWrite + cacheRead * p.cacheRead) / 1e6;
}

json toolsFor(const std::vector<control::CoreFunction>& functions, bool knowledge) {
  return anthropicTools(catalog(functions, knowledge));
}

json systemFor(const doc::Document& song, const doc::PCoreCatalog* pcore, const std::string& mode, bool gateway) {
  std::string text = kSystem;
  if (gateway)
    text += "\n\nAs ações de escrita (track, pcore, shape, arrange, plays, mix...) fazem-se com a ferramenta fazer {acao, dados}. "
            "Antes de usar uma ação pela primeira vez, pede o que ela recebe com ajuda {tema: a ação}; pede várias de uma vez se precisares.";
  if (mode == "readonly")
    text += "\n\nNeste chat só lês e explicas: o Diogo não deixou mudar a música, as ferramentas de escrita não estão disponíveis.";
  if (mode == "trusted")
    text += "\n\nNeste chat o Diogo confia nas tuas propostas: aplicam-se sozinhas quando acabas (continua a ser um só passo de undo).";
  return json::array({{{"type", "text"}, {"text", text}},
                      {{"type", "text"}, {"text", "Ficha dos estilos:\n" + styleCard(song, pcore).dump()}, {"cache_control", {{"type", "ephemeral"}}}}});
}

Chat::Chat(Config config, std::vector<control::CoreFunction> functions, const doc::PCoreCatalog* pcore, const Knowledge* knowledge,
           const Memory* memory)
    : config_(std::move(config)), functions_(std::move(functions)), pcore_(pcore), knowledge_(knowledge), memory_(memory),
      actions_(catalog(functions_, knowledge != nullptr, memory != nullptr, (bool)config_.listen)) {
  for (const auto& a : actions_)
    if (decide(config_.permissions, a) != Verdict::deny) offered_.push_back(a);
  tools_ = anthropicTools(config_.gateway ? gateway(offered_) : offered_);
}

double Chat::cost(const Usage& u) const {
  if (config_.pricesSet) return u.costUsd(config_.prices);
  return config_.provider.kind == "anthropic" ? u.costUsd(config_.model) : 0.0;
}

void Chat::restore(const json& messages) {
  reset();
  if (!messages.is_array()) return;
  messages_ = messages;
  // Valid only when it ends with the model's answer that asks for nothing more.
  while (!messages_.empty()) {
    const json& last = messages_.back();
    bool calls = false;
    if (last.value("role", std::string()) == "assistant" && last.contains("content") && last["content"].is_array())
      for (const auto& b : last["content"]) calls |= b.value("type", std::string()) == "tool_use";
    if (last.value("role", std::string()) == "assistant" && !calls) break;
    messages_.erase(messages_.size() - 1);
  }
  askStart_ = messages_.size();
}

void Chat::reset() {
  messages_ = json::array();
  askStart_ = 0;
  calls_ = json::array();
  asked_ = 0;
  log_.clear();
  reply_.clear();
  error_.clear();
  summary_.clear();
  steps_ = 0;
}

json Chat::request(const doc::Document& song) const {
  json r = {{"model", config_.model},
          {"max_tokens", config_.maxTokens},
          {"system", system_.is_null() ? systemFor(song, pcore_, config_.permissions.mode, config_.gateway) : system_},
          {"tools", tools_},
          {"messages", messages_},
          {"output_config", {{"effort", config_.effort}}},
          {"cache_control", {{"type", "ephemeral"}}}};
  if (config_.stream && config_.provider.kind != "openai") r["stream"] = true;
  return r;
}

json Chat::withZone(json proposal) const {
  if (zone_.active()) proposal["zone"] = zone_.toJson();
  return proposal;
}

std::string zoneBlock(const doc::Document& song, const control::Zone& zone) {
  if (!zone.active()) return "";
  const int64_t bar = 4 * doc::kPpq;
  const int64_t from = zone.startTick / bar + 1, to = (zone.endTick + bar - 1) / bar;
  std::string tracks;
  for (const auto& id : zone.tracks)
    if (const doc::Track* t = song.findTrack(id)) tracks += (tracks.empty() ? "" : ", ") + t->name;
  std::string sections;
  for (const auto& s : song.sections)
    if (s.startTick < zone.endTick && s.startTick + s.lengthTicks > zone.startTick)
      sections += (sections.empty() ? "" : ", ") + s.name + " (" + std::to_string(s.startTick / bar + 1) + "-" +
                  std::to_string((s.startTick + s.lengthTicks) / bar) + ")";
  return "Seleção do Diogo: compassos " + std::to_string(from) + "-" + std::to_string(to) + ", pistas: " + tracks + "." +
         (sections.empty() ? std::string() : " Secções nesta zona: " + sections + ".") +
         " Muda só estas pistas (ou pistas novas que cries para aqui), só nestes compassos: usa \"section\": \"" + control::kZoneSection +
         "\" para escrever ou subir aqui (o P-Core sem secção também escreve só aqui). O resto da música fica como está; "
         "o Core deixa de fora o que sair da seleção.";
}

json Chat::ask(const std::string& text, const doc::Document& song, const control::Zone* zone) {
  zone_ = zone ? *zone : control::Zone{};
  calls_ = json::array();
  asked_ = 0;
  log_.clear();
  reply_.clear();
  error_.clear();
  summary_.clear();
  steps_ = 0;
  usage_ = Usage{};
  ++proposalNo_;
  askStart_ = messages_.size();
  shown_.clear();
  // The song does not change while the model works (its edits are only proposed): one system for the whole message.
  system_ = systemFor(song, pcore_, config_.permissions.mode, config_.gateway);
  std::string content = text;
  if (const std::string z = zoneBlock(song, zone_); !z.empty()) content += "\n\n" + z;
  // The memory: the recipes most like this request go with it (as data), unless the permissions say no.
  if (memory_)
    if (const Action* a = findAction(actions_, "lembrar_receitas"); a && decide(config_.permissions, *a) != Verdict::deny) {
      const auto hits = memory_->search(text, doc::songStyle(song), nowMs());
      for (const auto& r : hits) shown_.push_back(r.id);
      if (!hits.empty()) content += "\n\n" + recipesBlock(hits);
    }
  // Tool results left from a message that stopped early go in the same turn as the new text (one user turn).
  if (!messages_.empty() && messages_.back().value("role", std::string()) == "user" && messages_.back()["content"].is_array())
    messages_.back()["content"].push_back({{"type", "text"}, {"text", content}});
  else
    messages_.push_back({{"role", "user"}, {"content", content}});
  summary_ = text.substr(0, 120);
  ++steps_;
  return request(song);
}

json Chat::runTool(const std::string& name, const json& input, const doc::Document& song) {
  if (config_.gateway && name == "ajuda") return gatewayHelp(offered_, input.value("tema", std::string()));
  if (config_.gateway && name == "fazer") {
    const std::string inner = input.value("acao", std::string());
    if (inner == "ajuda" || inner == "fazer") return {{"error", "fazer takes one of Core's write actions"}};
    return runTool(inner, input.contains("dados") && input["dados"].is_object() ? input["dados"] : json::object(), song);
  }
  const Action* action = findAction(actions_, name);
  if (!action) return {{"error", "unknown tool " + name}};
  const Verdict verdict = decide(config_.permissions, *action);
  if (verdict == Verdict::deny) {
    log_.push_back("not allowed: " + name);
    return {{"error", "not allowed: Diogo's permissions deny " + name}};
  }
  if (name == "ler_musica") {
    json out = songForAi(song);
    if (!zone_.active()) return out;
    // With a selection: the selected tracks whole, the others by name only (they cannot change; smaller requests).
    json others = json::array();
    json tracks = json::array();
    for (const auto& t : out["tracks"])
      if (std::find(zone_.tracks.begin(), zone_.tracks.end(), t.value("id", std::string())) != zone_.tracks.end()) tracks.push_back(t);
      else others.push_back(t.value("name", std::string()));
    out["tracks"] = tracks;
    out["otherTracks"] = others;
    out["selection"] = {{"startBar", zone_.startTick / (4 * doc::kPpq) + 1}, {"endBar", zone_.endTick / (4 * doc::kPpq)}};
    return out;
  }
  if (name == "lembrar_receitas") {
    if (!memory_) return {{"error", "the memory is not open"}};
    json out = json::array();
    for (const auto& r : memory_->search(input.value("pergunta", std::string()), input.value("estilo", std::string()), nowMs(), 5)) {
      if (std::find(shown_.begin(), shown_.end(), r.id) == shown_.end()) shown_.push_back(r.id);
      out.push_back({{"pedido", r.request}, {"estado", r.status == "liked" ? "gostou" : "aplicou"}, {"estilo", r.style}, {"calls", r.calls},
                     {"nota_do_diogo", r.note}});
    }
    if (out.empty()) return {{"result", "nothing in the memory for that"}};
    return {{"recipes", out}};
  }
  if (name == "consultar_manual") {
    if (!knowledge_) return {{"error", "the knowledge base is not installed"}};
    json hits = json::array();
    for (const auto& h : knowledge_->search(input.value("pergunta", std::string()), input.value("estilo", std::string()), 4)) {
      std::string text = h.text.size() > 1800 ? h.text.substr(0, 1800) + "…" : h.text;
      hits.push_back({{"title", h.title}, {"style", h.style}, {"text", text}});
    }
    if (hits.empty()) return {{"result", "nothing found: try other words"}};
    return {{"excerpts", hits}};
  }
  if (name == "ler_kick_bass") {
    if (!config_.listen) return {{"error", "Core cannot listen here"}};
    auto same = [](std::string a, std::string b) {
      for (auto& c : a) c = (char)std::tolower((unsigned char)c);
      for (auto& c : b) c = (char)std::tolower((unsigned char)c);
      return !a.empty() && a == b;
    };
    std::string sec = input.value("secção", std::string());
    if (sec.empty()) sec = input.value("secao", std::string());
    const doc::Section* section = nullptr;
    for (const auto& s : song.sections)
      if (same(s.name, sec) || s.id == sec) section = &s;
    if (!section) return {{"error", "no section called \"" + sec + "\""}};
    // The tracks: by the names given, else the first kick and the first bass by their type.
    auto find = [&](const std::string& given, const char* type) -> const doc::Track* {
      for (const auto& t : song.tracks)
        if (!given.empty() ? same(t.name, given) : control::trackType(t) == type) return &t;
      return nullptr;
    };
    const doc::Track* kick = find(input.value("kick", std::string()), "kick");
    const doc::Track* bass = find(input.value("bass", std::string()), "bass");
    if (!kick || !bass) return {{"error", std::string("no ") + (!kick ? "kick" : "bass") + " track: name it (\"kick\" / \"bass\")"}};
    const int64_t start = section->startTick;
    const int64_t end = std::min({section->startTick + section->lengthTicks, start + 8 * 4 * doc::kPpq, song.lengthTicks});
    std::vector<float> k, b;
    double sr = 48000;
    const std::string why = config_.listen(kick->id, bass->id, start, end, k, b, sr);
    if (!why.empty()) return {{"error", "could not listen: " + why}};
    json out = analyzeKickBass(k, b, sr, song.bpm).toJson();
    out["kick"] = kick->name;
    out["bass"] = bass->name;
    out["compassos"] = (double)(end - start) / (4.0 * doc::kPpq);
    return out;
  }
  if (name == "formas_pcore") {
    std::string style = input.value("style", std::string());
    if (style.empty() && pcore_)
      if (const doc::PCoreStyle* s = doc::pcoreStyleFor(*pcore_, doc::songStyle(song))) style = s->id;
    return shapesFor(pcore_, input.value("part", std::string()), style);
  }
  if (action->kind == Kind::read) return {{"error", "unknown tool " + name}};
  json call = input.is_object() ? input : json::object();
  call["fn"] = action->function;
  json all = calls_;
  all.push_back(call);
  const control::Proposal before = control::validateProposal(withZone({{"calls", calls_}}), functions_, song, nullptr);
  const control::Proposal p = control::validateProposal(withZone({{"calls", all}}), functions_, song, nullptr);
  std::vector<std::string> fresh(p.problems.begin() + std::min(p.problems.size(), before.problems.size()), p.problems.end());
  if (p.calls.size() > before.calls.size()) {
    calls_.push_back(call);
    if (verdict == Verdict::ask) ++asked_;
    const control::Call& made = p.calls.back();
    std::string line = "ok: " + control::describeCall(made);
    log_.push_back(line);
    json out = {{"result", line}};
    if (!made.warnings.empty()) out["warnings"] = made.warnings;
    if (!fresh.empty()) out["problems"] = fresh;
    return out;
  }
  std::string why;
  for (const auto& s : fresh) why += (why.empty() ? "" : "; ") + s;
  if (why.empty()) why = "not accepted";
  log_.push_back("left out: " + why);
  return {{"result", "left out"}, {"problems", fresh}};
}

std::string problemKind(const std::string& why) {
  auto has = [&](const char* w) { return why.find(w) != std::string::npos; };
  if (has("not allowed")) return "permission";
  if (has("no section")) return "section";
  if (has("is outside") || has("not a number") || has("variationEvery")) return "range";
  if (has("is not one of") || has("is not a function") || has("unknown tool") || has("no track called") || has("no track '") ||
      has("no preset") || has("no knob") || has("no sample") || has("no effect"))
    return "name";
  if (has("is needed") || has("given") || has("is not clear")) return "missing";
  if (has("new track") || has("plays no notes") || has("needs a track") || has("on a track") || has("on the master") ||
      has("the master has") || has("has no audio") || has("cannot run") || has("not a song function"))
    return "dependency";
  return "other";
}


bool Chat::overBudget() {
  if (config_.maxUsdPerMessage <= 0 || cost(usage_) < config_.maxUsdPerMessage) return false;
  char b[64];
  std::snprintf(b, sizeof b, "%.2f", config_.maxUsdPerMessage);
  error_ = std::string("stopped: this message reached its cost limit ($") + b + ", Settings)";
  return true;
}

json Chat::onResponse(const json& raw, const doc::Document& song) {
  const Turn turn = readResponse(config_.provider, raw);
  if (!turn.error.empty()) {
    error_ = turn.error;
    abandon();
    return nullptr;
  }
  usage_.add(turn.usage);
  total_.add(turn.usage);
  const json& content = turn.content;
  messages_.push_back({{"role", "assistant"}, {"content", content}});
  std::string text;
  json results = json::array();
  for (const auto& b : content) {
    const std::string type = b.value("type", std::string());
    if (type == "text") text += (text.empty() ? "" : "\n") + b.value("text", std::string());
    if (type == "tool_use") {
      // A call in an answer that stopped for another reason (cut off, refused) may be half written: it is not run,
      // but it is answered, or the next request would be refused.
      const json out = turn.stop == "tool_use" ? runTool(b.value("name", std::string()), b.contains("input") ? b["input"] : json::object(), song)
                                               : json{{"error", "not run: the answer stopped (" + turn.stop + ")"}};
      results.push_back({{"type", "tool_result"}, {"tool_use_id", b.value("id", std::string())}, {"content", out.dump()}});
    }
  }
  if (!text.empty()) reply_ = text;
  const std::string& stop = turn.stop;
  if (stop == "max_tokens") error_ = "the answer was cut off (max_tokens)";
  if (stop == "refusal") error_ = "the model declined";
  if (stop != "tool_use" && !results.empty()) messages_.push_back({{"role", "user"}, {"content", results}});
  if (stop == "tool_use" && !results.empty()) {
    messages_.push_back({{"role", "user"}, {"content", results}});
    if (steps_ >= config_.maxSteps) {
      error_ = "stopped after " + std::to_string(steps_) + " requests";
      // The tool results stay in the conversation: answer them on the next message.
      return nullptr;
    }
    if (overBudget()) return nullptr;
    ++steps_;
    return request(song);
  }
  if (stop == "pause_turn") {
    ++steps_;
    return steps_ > config_.maxSteps || overBudget() ? json(nullptr) : request(song);
  }
  if (!reply_.empty()) summary_ = reply_.substr(0, 200);
  return nullptr;
}

void Chat::abandon() {
  while (messages_.size() > askStart_) messages_.erase(messages_.size() - 1);
  // The new text may have joined a turn of earlier tool results: take it out again.
  if (!messages_.empty() && messages_.back().value("role", std::string()) == "user" && messages_.back()["content"].is_array()) {
    json& c = messages_.back()["content"];
    while (!c.empty() && c.back().value("type", std::string()) == "text") c.erase(c.size() - 1);
  }
  calls_ = json::array();
  asked_ = 0;
}

json Chat::episode() const {
  std::string system;
  for (const auto& b : system_.is_array() ? system_ : json::array())
    if (b.value("type", std::string()) == "text") system += (system.empty() ? "" : "\n\n") + b.value("text", std::string());
  return {{"format", "core-ai-episode"},
          {"version", 1},
          {"provider", config_.provider.kind},
          {"model", config_.model},
          {"effort", config_.effort},
          {"mode", config_.permissions.mode},
          {"toolsView", config_.gateway ? "gateway" : "full"},
          {"system", system},
          {"tools", mcpTools(offered_)},
          {"messages", messages_},
          {"turnStart", askStart_},
          {"zone", zone_.active() ? zone_.toJson() : json(nullptr)},
          {"shown", shown_},
          {"log", log_}};
}

json Chat::proposal() const {
  // Applied by itself only when every call in it is allowed (trusted); else it waits for Apply.
  const bool automatic = !calls_.empty() && asked_ == 0;
  return withZone({{"id", "ai-" + std::to_string(proposalNo_)}, {"summary", summary_}, {"calls", calls_}, {"auto", automatic}});
}

}  // namespace core::ai
