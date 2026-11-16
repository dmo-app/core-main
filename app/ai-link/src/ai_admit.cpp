#include "ai_admit.hpp"
#include "ai_scope.hpp"

namespace core::ai {

using json = nlohmann::json;

Admission admitProposal(const json& raw, const std::vector<control::CoreFunction>& functions, const doc::Document& song,
                        const doc::SampleIndex* samples, const Permissions& permissions, const control::Zone& zone) {
  Admission a;
  auto refuse = [&](const std::string& why) {
    a.why = why;
    return a;
  };
  // 1. The shape.
  if (!raw.is_object()) return refuse("the proposal is not an object");
  if (raw.contains("id") && (!raw["id"].is_string() || raw["id"].get_ref<const std::string&>().size() > 64)) return refuse("its id is not short text");
  if (raw.contains("summary") && (!raw["summary"].is_string() || raw["summary"].get_ref<const std::string&>().size() > 300))
    return refuse("its summary is not short text");
  if (!raw.contains("calls") || !raw["calls"].is_array()) return refuse("it has no list of calls");
  const json& calls = raw["calls"];
  if (calls.empty()) return refuse("it has no calls");
  if (calls.size() > kMaxCalls) return refuse("it has more than " + std::to_string(kMaxCalls) + " calls");
  // 2 and 3. The scope and the permissions, call by call.
  const std::vector<control::CoreFunction> exposed = exposedFunctions(functions);
  const std::vector<Action> actions = catalog(exposed, false);
  for (size_t i = 0; i < calls.size(); ++i) {
    const json& c = calls[i];
    const std::string at = "call " + std::to_string(i + 1) + ": ";
    if (!c.is_object() || !c.contains("fn") || !c["fn"].is_string()) return refuse(at + "not a call (no \"fn\")");
    const std::string& fn = c["fn"].get_ref<const std::string&>();
    if (!inCreationScope(fn)) return refuse(at + "\"" + fn.substr(0, 40) + "\" is not something the AI may do in Core (only music creation)");
    const Action* act = findAction(actions, toolName(fn));
    if (!act) return refuse(at + "\"" + fn + "\" cannot run on this computer now");
    if (decide(permissions, *act) == Verdict::deny) return refuse(at + "\"" + fn + "\" is denied by the AI's permissions");
  }
  // 4. Core's zone, never the AI's.
  json checked = raw;
  checked.erase("auto");
  if (zone.active()) checked["zone"] = zone.toJson();
  else checked.erase("zone");
  // 5. Core's own check: every call must fit, or nothing is admitted.
  // Against the caller's list (it outlives the proposal: each call points at its function); the scope is checked above.
  control::Proposal p = control::validateProposal(checked, functions, song, samples);
  if (!p.problems.empty()) return refuse(p.problems.front());
  if (p.calls.size() != calls.size()) return refuse("only " + std::to_string(p.calls.size()) + " of its " + std::to_string(calls.size()) + " calls fit the song");
  a.ok = true;
  a.proposal = std::move(p);
  a.raw = std::move(checked);
  return a;
}

std::string changedSince(const json& proposal, const control::Proposal& now) {
  const size_t asked = proposal.is_object() && proposal.contains("calls") && proposal["calls"].is_array() ? proposal["calls"].size() : 0;
  if (now.calls.size() >= asked) return "";
  return "the song changed since the proposal: " + std::to_string(asked - now.calls.size()) + " of its calls no longer fit (" +
         (now.problems.empty() ? std::string("ask again") : now.problems.front()) + "); nothing was applied";
}

}  // namespace core::ai
