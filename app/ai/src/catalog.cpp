#include "catalog.hpp"
#include <cctype>
#include <cstdio>

namespace core::ai {

namespace {

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

std::string trackHint(const control::CoreFunction& f) {
  if (f.family == "mix" || f.family == "open") return "the track's name, or \"master\"";
  if (f.family == "arrange") return "the track's name (leave it out for every track)";
  return "the track's name (a track made by \"track\" in this chat counts)";
}

Action readAction(std::string name, std::string description, json schema) {
  Action a;
  a.name = std::move(name);
  a.description = std::move(description);
  a.schema = std::move(schema);
  a.kind = Kind::read;
  a.family = "read";
  return a;
}

Action editAction(const control::CoreFunction& f) {
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
  Action e;
  e.name = toolName(f.id);
  e.description = f.name + ": " + f.description;
  e.schema = schema;
  e.kind = Kind::edit;
  e.function = f.id;
  e.family = f.family;
  return e;
}

bool names(const std::set<std::string>& list, const Action& a) {
  return list.count("*") || list.count(a.name) || (!a.family.empty() && list.count(a.family));
}

}  // namespace

std::string toolName(const std::string& functionId) {
  std::string out;
  for (unsigned char c : functionId) out += (std::isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
  return out.substr(0, 64);
}

std::vector<Action> catalog(const std::vector<control::CoreFunction>& functions, bool knowledge, bool memory) {
  std::vector<Action> out;
  if (memory)
    out.push_back(readAction(
        "lembrar_receitas",
        "Procura na memória do Core as receitas que o Diogo já aplicou ou de que gostou (pedido, estilo e as chamadas do Core). "
        "As mais parecidas com o pedido já vêm com ele; usa isto para procurar outras.",
        {{"type", "object"},
         {"properties", {{"pergunta", {{"type", "string"}, {"description", "palavras do pedido ou da parte (\"baixo KBBB drop\")"}}},
                         {"estilo", {{"type", "string"}, {"description", "o estilo (opcional)"}}}}},
         {"required", {"pergunta"}}}));
  if (knowledge)
    out.push_back(readAction(
        "consultar_manual",
        "Consulta a base local do CORE-MANUAL: como fazer música em cada estilo (BPM, kick, KBBB, baixo, hats, "
        "arranjo, energia, mistura, técnicas). Devolve os excertos que melhor respondem.",
        {{"type", "object"},
         {"properties", {{"pergunta", {{"type", "string"}, {"description", "o que queres saber, em palavras-chave"}}},
                         {"estilo", {{"type", "string"}, {"description", "darkpsycore, hitech, psytrance, slambient (opcional)"}}}}},
         {"required", {"pergunta"}}}));
  out.push_back(readAction("ler_musica",
                           "Lê a música aberta no Core: nome, estilo, BPM, compassos, secções e pistas (parte, padrões, onde tocam, plugin, mistura).",
                           {{"type", "object"}, {"properties", json::object()}}));
  out.push_back(readAction("formas_pcore", "As formas (padrões) do P-Core para uma parte, as do estilo primeiro, com a grelha e os passos.",
                           {{"type", "object"},
                            {"properties", {{"part", {{"type", "string"}, {"description", "kick, bass, hats, cymbals, perc, build, lead, arp, pad, fx"}}},
                                            {"style", {{"type", "string"}, {"description", "o id do estilo (opcional)"}}}}},
                            {"required", {"part"}}}));
  for (const auto& f : functions)
    if (f.available) out.push_back(editAction(f));
  return out;
}

const Action* findAction(const std::vector<Action>& actions, const std::string& name) {
  for (const auto& a : actions)
    if (a.name == name) return &a;
  return nullptr;
}

json anthropicTools(const std::vector<Action>& actions) {
  json tools = json::array();
  for (const auto& a : actions) tools.push_back({{"name", a.name}, {"description", a.description}, {"input_schema", a.schema}});
  if (!tools.empty()) tools.back()["cache_control"] = {{"type", "ephemeral"}};
  return tools;
}

json openAiTools(const std::vector<Action>& actions) {
  json tools = json::array();
  for (const auto& a : actions)
    tools.push_back({{"type", "function"}, {"function", {{"name", a.name}, {"description", a.description}, {"parameters", a.schema}}}});
  return tools;
}

json mcpTools(const std::vector<Action>& actions) {
  json tools = json::array();
  for (const auto& a : actions) tools.push_back({{"name", a.name}, {"description", a.description}, {"inputSchema", a.schema}});
  return tools;
}

std::vector<Action> gateway(const std::vector<Action>& offered) {
  std::vector<Action> out;
  json names = json::array();
  for (const auto& a : offered)
    if (a.kind == Kind::read) out.push_back(a);
    else names.push_back(a.name);
  if (names.empty()) return out;
  out.push_back(readAction("ajuda",
                           "O que uma ação de escrita do Core recebe (o esquema dos argumentos). Sem tema: a lista das ações. "
                           "Pede antes de usar uma ação pela primeira vez.",
                           {{"type", "object"},
                            {"properties", {{"tema", {{"type", "string"}, {"description", "o nome da ação (\"pcore\") ou da família; vazio = a lista"}}}}}}));
  Action fazer;
  fazer.name = "fazer";
  fazer.description = "Faz uma ação de escrita do Core (entra na proposta, verificada como sempre). Os argumentos da ação vão em \"dados\", "
                      "como \"ajuda\" os descreve.";
  fazer.schema = {{"type", "object"},
                  {"properties", {{"acao", {{"type", "string"}, {"enum", names}}}, {"dados", {{"type", "object"}}}}},
                  {"required", {"acao", "dados"}}};
  fazer.kind = Kind::edit;
  fazer.family = "gateway";
  out.push_back(fazer);
  return out;
}

json gatewayHelp(const std::vector<Action>& offered, const std::string& topic) {
  json out = json::array();
  for (const auto& a : offered) {
    if (a.kind == Kind::read) continue;
    if (topic.empty()) {
      std::string line = a.description.substr(0, a.description.find('.'));
      out.push_back({{"acao", a.name}, {"o_que_faz", line.substr(0, 160)}});
    } else if (a.name == topic || a.family == topic) {
      out.push_back({{"acao", a.name}, {"descricao", a.description}, {"dados", a.schema}});
    }
  }
  if (out.empty()) return {{"error", "no action or family named " + topic + "; ask with no topic for the list"}};
  return {{"acoes", out}};
}

const char* verdictName(Verdict v) { return v == Verdict::allow ? "allow" : v == Verdict::deny ? "deny" : "ask"; }

const std::vector<std::string>& permissionModes() {
  static const std::vector<std::string> m = {"ask", "readonly", "trusted"};
  return m;
}

Verdict decide(const Permissions& p, const Action& a) {
  // A read-only chat never changes the song, whatever the lists say.
  if (p.mode == "readonly") return a.kind == Kind::read ? Verdict::allow : Verdict::deny;
  if (names(p.deny, a)) return Verdict::deny;
  if (names(p.ask, a)) return Verdict::ask;
  if (names(p.allow, a)) return Verdict::allow;
  if (a.kind == Kind::read) return Verdict::allow;
  return p.mode == "trusted" ? Verdict::allow : Verdict::ask;
}

Permissions permissionsFrom(const json& settings, std::vector<std::string>* problems) {
  Permissions p;
  if (!settings.is_object() || !settings.contains("permissions")) return p;
  const json& j = settings["permissions"];
  auto problem = [&](const std::string& s) {
    if (problems) problems->push_back("permissions: " + s);
  };
  if (!j.is_object()) {
    problem("not an object");
    return p;
  }
  if (j.contains("mode")) {
    const std::string m = j["mode"].is_string() ? j["mode"].get<std::string>() : "";
    bool known = false;
    for (const auto& k : permissionModes()) known |= k == m;
    if (known) p.mode = m;
    else problem("unknown mode (ask, readonly or trusted)");
  }
  auto list = [&](const char* key, std::set<std::string>& out) {
    if (!j.contains(key)) return;
    if (!j[key].is_array()) return problem(std::string(key) + " is not a list");
    for (const auto& v : j[key])
      if (v.is_string() && !v.get<std::string>().empty()) out.insert(v.get<std::string>());
      else problem(std::string(key) + ": a name that is not text");
  };
  list("allow", p.allow);
  list("deny", p.deny);
  list("ask", p.ask);
  return p;
}

}  // namespace core::ai
