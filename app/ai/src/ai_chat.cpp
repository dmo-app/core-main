#include "ai_chat.hpp"
#include <algorithm>
#include <cctype>
#include "samples.hpp"
#include "score.hpp"

namespace core::ai {

namespace {

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
    "para kick, baixo e hats, para ficarem alinhados. Para a estrutura: shape, depois arrange. Para mudar uma parte: "
    "plays (tirar ou pôr uma pista numa secção) e pcore com mode alter ou outra forma.\n"
    "- Usa os ids exactos das opções (formas, estilos, escalas). Para ver as formas de uma parte, usa formas_pcore.\n"
    "- Quando não souberes como se faz algo num estilo (BPM, KBBB, energia, arranjo, mistura), consulta o manual "
    "(consultar_manual) com poucas palavras-chave, antes de propor.\n"
    "- Uma pista nova não tem som até o Diogo escolher o plugin (VST3); diz-lhe isso quando criares pistas.\n"
    "- No fim, responde com uma ou duas frases: o que propuseste e o que ele deve ouvir. Esse texto é o título da proposta.";

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

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

std::string trackHint(const control::CoreFunction& f) {
  if (f.family == "mix" || f.family == "open") return "the track's name, or \"master\"";
  if (f.family == "arrange") return "the track's name (leave it out for every track)";
  return "the track's name (a track made by \"track\" in this chat counts)";
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

std::string toolName(const std::string& functionId) {
  std::string out;
  for (unsigned char c : functionId) out += (std::isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
  return out.substr(0, 64);
}

json toolsFor(const std::vector<control::CoreFunction>& functions, bool knowledge) {
  json tools = json::array();
  if (knowledge)
    tools.push_back({{"name", "consultar_manual"},
                     {"description", "Consulta a base local do CORE-MANUAL: como fazer música em cada estilo (BPM, kick, KBBB, baixo, hats, "
                                     "arranjo, energia, mistura, técnicas). Devolve os excertos que melhor respondem."},
                     {"input_schema", {{"type", "object"},
                                       {"properties", {{"pergunta", {{"type", "string"}, {"description", "o que queres saber, em palavras-chave"}}},
                                                       {"estilo", {{"type", "string"}, {"description", "darkpsycore, hitech, psytrance, slambient (opcional)"}}}}},
                                       {"required", {"pergunta"}}}}});
  tools.push_back({{"name", "ler_musica"},
                   {"description", "Lê a música aberta no Core: nome, estilo, BPM, compassos, secções e pistas (parte, padrões, onde tocam, plugin, mistura)."},
                   {"input_schema", {{"type", "object"}, {"properties", json::object()}}}});
  tools.push_back({{"name", "formas_pcore"},
                   {"description", "As formas (padrões) do P-Core para uma parte, as do estilo primeiro, com a grelha e os passos."},
                   {"input_schema", {{"type", "object"},
                                     {"properties", {{"part", {{"type", "string"}, {"description", "kick, bass, hats, cymbals, perc, build, lead, arp, pad, fx"}}},
                                                     {"style", {{"type", "string"}, {"description", "o id do estilo (opcional)"}}}}},
                                     {"required", {"part"}}}}});
  for (const auto& f : functions) {
    if (!f.available) continue;
    json props = json::object();
    json required = json::array();
    if (f.family != "track" && f.family != "shape") props["on"] = {{"type", "string"}, {"description", trackHint(f)}};
    if (f.rises || f.family == "pcore" || f.family == "arrange" || f.family == "plays")
      props["section"] = {{"type", "string"}, {"description", "the section's name (\"Drop\")"}};
    if (!f.presets.empty()) props["preset"] = {{"type", "string"}, {"enum", f.presets}};
    if (f.family == "track") props["name"] = {{"type", "string"}, {"description", "the track's name (else the part's)"}};
    if (f.family == "open") props["what"] = {{"type", "string"}, {"description", "instrument, patterns, mixer, score, effects or an effect's name"}};
    for (const auto& [key, values] : f.options) {
      json p = {{"type", "string"}};
      if (!values.empty()) p["enum"] = values;
      props[key] = p;
    }
    if (f.family == "track") required.push_back("part");
    if (f.family == "shape") required.push_back("shape");
    if (f.family == "plays") required = {"on", "section", "play"};
    if (f.family == "pcore") required.push_back("on");
    if (!f.args.empty()) {
      json args = json::object();
      for (const auto& a : f.args) {
        std::string d = num(a.min) + ".." + num(a.max) + (a.unit.empty() ? "" : " " + a.unit) + ", default " + num(a.def);
        if (!a.hint.empty()) d += ": " + a.hint;
        args[a.id] = {{"type", "number"}, {"description", a.name + " (" + d + ")"}};
      }
      props["args"] = {{"type", "object"}, {"properties", args}, {"additionalProperties", false}};
    }
    json schema = {{"type", "object"}, {"properties", props}};
    if (!required.empty()) schema["required"] = required;
    std::string description = f.name + ": " + f.description;
    tools.push_back({{"name", toolName(f.id)}, {"description", description}, {"input_schema", schema}});
  }
  tools.back()["cache_control"] = {{"type", "ephemeral"}};
  return tools;
}

json systemFor(const doc::Document& song, const doc::PCoreCatalog* pcore) {
  return json::array({{{"type", "text"}, {"text", kSystem}},
                      {{"type", "text"}, {"text", "Ficha dos estilos:\n" + styleCard(song, pcore).dump()}, {"cache_control", {{"type", "ephemeral"}}}}});
}

Chat::Chat(Config config, std::vector<control::CoreFunction> functions, const doc::PCoreCatalog* pcore, const Knowledge* knowledge)
    : config_(std::move(config)), functions_(std::move(functions)), pcore_(pcore), knowledge_(knowledge), tools_(toolsFor(functions_, knowledge != nullptr)) {}

double Chat::cost(const Usage& u) const {
  if (config_.pricesSet) return u.costUsd(config_.prices);
  return config_.provider.kind == "anthropic" ? u.costUsd(config_.model) : 0.0;
}

void Chat::reset() {
  messages_ = json::array();
  calls_ = json::array();
  log_.clear();
  reply_.clear();
  error_.clear();
  summary_.clear();
  steps_ = 0;
}

json Chat::request(const doc::Document& song) const {
  return {{"model", config_.model},
          {"max_tokens", config_.maxTokens},
          {"system", systemFor(song, pcore_)},
          {"tools", tools_},
          {"messages", messages_},
          {"output_config", {{"effort", config_.effort}}},
          {"cache_control", {{"type", "ephemeral"}}}};
}

json Chat::ask(const std::string& text, const doc::Document& song) {
  calls_ = json::array();
  log_.clear();
  reply_.clear();
  error_.clear();
  summary_.clear();
  steps_ = 0;
  usage_ = Usage{};
  ++proposalNo_;
  messages_.push_back({{"role", "user"}, {"content", text}});
  summary_ = text.substr(0, 120);
  ++steps_;
  return request(song);
}

json Chat::runTool(const std::string& name, const json& input, const doc::Document& song) {
  if (name == "ler_musica") return songForAi(song);
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
  if (name == "formas_pcore") {
    std::string style = input.value("style", std::string());
    if (style.empty() && pcore_)
      if (const doc::PCoreStyle* s = doc::pcoreStyleFor(*pcore_, doc::songStyle(song))) style = s->id;
    return shapesFor(pcore_, input.value("part", std::string()), style);
  }
  const control::CoreFunction* f = nullptr;
  for (const auto& fn : functions_)
    if (toolName(fn.id) == name) f = &fn;
  if (!f) return {{"error", "unknown tool " + name}};
  json call = input.is_object() ? input : json::object();
  call["fn"] = f->id;
  json all = calls_;
  all.push_back(call);
  const control::Proposal before = control::validateProposal({{"calls", calls_}}, functions_, song, nullptr);
  const control::Proposal p = control::validateProposal({{"calls", all}}, functions_, song, nullptr);
  std::vector<std::string> fresh(p.problems.begin() + std::min(p.problems.size(), before.problems.size()), p.problems.end());
  if (p.calls.size() > before.calls.size()) {
    calls_.push_back(call);
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

json Chat::onResponse(const json& raw, const doc::Document& song) {
  const Turn turn = readResponse(config_.provider, raw);
  if (!turn.error.empty()) {
    error_ = turn.error;
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
      const json out = runTool(b.value("name", std::string()), b.contains("input") ? b["input"] : json::object(), song);
      results.push_back({{"type", "tool_result"}, {"tool_use_id", b.value("id", std::string())}, {"content", out.dump()}});
    }
  }
  if (!text.empty()) reply_ = text;
  const std::string& stop = turn.stop;
  if (stop == "max_tokens") error_ = "the answer was cut off (max_tokens)";
  if (stop == "refusal") error_ = "the model declined";
  if (stop == "tool_use" && !results.empty()) {
    messages_.push_back({{"role", "user"}, {"content", results}});
    if (steps_ >= config_.maxSteps) {
      error_ = "stopped after " + std::to_string(steps_) + " requests";
      // The tool results stay in the conversation: answer them on the next message.
      return nullptr;
    }
    ++steps_;
    return request(song);
  }
  if (stop == "pause_turn") {
    ++steps_;
    return steps_ > config_.maxSteps ? json(nullptr) : request(song);
  }
  if (!reply_.empty()) summary_ = reply_.substr(0, 200);
  return nullptr;
}

json Chat::proposal() const {
  return {{"id", "ai-" + std::to_string(proposalNo_)}, {"summary", summary_}, {"calls", calls_}};
}

}  // namespace core::ai
