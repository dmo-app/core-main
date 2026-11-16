// Everything the AI can do in Core, in ONE catalog (Diogo, 2026-11-16). The pattern comes from theDAW
// (github.com/gantasmo/thedaw, MIT, backend/modules/assistant/tool_catalog.py and permissions.py): one list of
// actions is the only source of truth, every provider's tool definitions are views of it (Anthropic, OpenAI-compatible,
// MCP), and one policy decides for each action: allow, deny or ask. No code was copied from theDAW, only the idea.
//
// In Core:
//   read  actions (ler_musica, formas_pcore, consultar_manual) only look; they run when the model calls them.
//   edit  actions are Core's calls (control/calls.hpp: pcore, shape, track, mix...). They never touch the song while the
//         model works: each is checked and joins the proposal.
//   allow  read: it runs.  edit: it joins the proposal, and a proposal of allowed edits only is applied by itself when
//          the answer ends (still ONE undo step).
//   ask    edit: it joins the proposal and waits for Apply (the default).
//   deny   the action is not offered to the model; if it is called anyway, the model is told no.
#pragma once
#include <set>
#include <string>
#include <vector>
#include "calls.hpp"

namespace core::ai {

using json = nlohmann::json;

enum class Kind { read, edit };

struct Action {
  std::string name;         // the tool's name ("ler_musica", "pcore", "fx_reverb")
  std::string description;
  json schema;              // JSON Schema of the input
  Kind kind = Kind::edit;
  std::string function;     // the Core function an edit makes ("" for a read)
  std::string family;       // its family ("pcore", "mix"...; "read" for a read)
};

// The tool name of a Core function ("pcore", "fx_reverb"...): letters, digits, _ and -.
std::string toolName(const std::string& functionId);

// The catalog: the read actions (consultar_manual only with the knowledge base), then one edit per function Core can
// run now (its text choices as enums, its knobs with their ranges).
std::vector<Action> catalog(const std::vector<control::CoreFunction>& functions, bool knowledge);
const Action* findAction(const std::vector<Action>& actions, const std::string& name);

// The views. Anthropic: {name, description, input_schema}, the last one with cache_control (tools and system are
// cached together). OpenAI: {"type": "function", "function": {name, description, parameters}}. MCP (tools/list):
// {name, description, inputSchema}.
json anthropicTools(const std::vector<Action>& actions);
json openAiTools(const std::vector<Action>& actions);
json mcpTools(const std::vector<Action>& actions);

// The permission policy.
enum class Verdict { allow, deny, ask };
const char* verdictName(Verdict v);

struct Permissions {
  // ask      reads allowed, edits wait for Apply (default)
  // readonly reads allowed, edits denied (the AI only answers and explains)
  // trusted  reads and edits allowed (a proposal of allowed edits is applied by itself)
  std::string mode = "ask";
  // Per action, over the mode: an action's name ("pcore"), its family ("mix") or "*". deny wins, then ask, then allow.
  std::set<std::string> allow, deny, ask;
};
const std::vector<std::string>& permissionModes();  // ask, readonly, trusted

Verdict decide(const Permissions& p, const Action& a);
// "permissions" of ai-settings.json: {"mode": "ask", "allow": [...], "deny": [...], "ask": [...]}. Bad values are left
// out, with why in problems.
Permissions permissionsFrom(const json& settings, std::vector<std::string>* problems = nullptr);

}  // namespace core::ai
