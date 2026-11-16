#include "role_tools.hpp"

#include <algorithm>
#include <cmath>
#include "edit.hpp"
#include "follow.hpp"

namespace core::doc {

namespace {
void checkSpan(const Document& doc, int64_t start, int64_t end) {
  if (start < 0 || end <= start || end > doc.lengthTicks) throw DocumentError("tool", "the span must be inside the song and end after it starts");
  if (end - start < kPpq / 4) throw DocumentError("tool", "the span is shorter than a 1/16");
}
// The role can be written: mapped, an effect role, and the macro has a parameter. Throws otherwise (before any change).
void checkMacro(const RoleMap& map, const std::string& role, const std::string& macro) {
  roleParamKey(map, role, "check", macro);
}
bool hasMacro(const RoleMap& map, const std::string& role, const std::string& macro) {
  const RoleBinding* b = map.find(role);
  if (!b) return false;
  auto it = b->params.find(macro);
  return it != b->params.end() && !it->second.empty();
}
std::string whyNot(const RoleMap& map, const std::string& role, const std::string& macro) {
  const RoleInfo* info = findRole(role);
  const std::string label = info ? info->label : role;
  if (!map.find(role)) return label + ": no plugin chosen (Settings > Roles)";
  const RoleMacro* m = info ? findRoleMacro(*info, macro) : nullptr;
  return label + ": no parameter for " + (m ? m->label : macro) + " (Settings > Roles)";
}
std::string percent(double v) { return std::to_string((int)std::lround(v * 100)) + "%"; }
std::string bars(int64_t ticks) {
  const double b = (double)ticks / (4.0 * kPpq);
  if (std::abs(b - std::round(b)) < 1e-9) return std::to_string((int64_t)std::round(b)) + (std::round(b) == 1 ? " bar" : " bars");
  return std::to_string((int64_t)std::round(b * 4)) + " beats";
}
}  // namespace

std::string writeRoleSweep(Document& doc, const std::string& trackId, const RoleMap& map, const std::string& role,
                           const std::string& macro, int64_t start, int64_t end, double from, double to, double curve) {
  checkSpan(doc, start, end);
  checkMacro(map, role, macro);
  if (!trackId.empty()) trackById(doc, trackId);  // throws for a missing track ("" = the master)
  const RoleBinding& b = *map.find(role);
  const double rest = roleRest(b, role, macro);
  const std::string effectId = ensureRoleEffect(doc, trackId, role, map);
  const std::string key = roleParamKey(map, role, effectId, macro);
  from = std::clamp(from, 0.0, 1.0);
  to = std::clamp(to, 0.0, 1.0);
  curve = std::clamp(curve, 0.1, 10.0);
  // A point every 1/16, at most 64 over the span (straight lines between them).
  const int64_t length = end - start;
  const int64_t step = std::max<int64_t>(kPpq / 4, length / 64);
  std::vector<AutomationPoint> points;
  for (int64_t t = start; t < end; t += step) {
    const double x = (double)(t - start) / (double)length;
    points.push_back({t, from + (to - from) * std::pow(x, curve), json::object()});
  }
  if (end - 1 > points.back().tick) points.push_back({end - 1, to, json::object()});  // the top, just before the drop
  setAutomationInRange(doc, trackId, key, start, end, points, rest);
  return key;
}

RoleToolResult toolSuck(Document& doc, const std::string& trackId, const RoleMap& map, int64_t dropTick, int64_t lengthTicks,
                        double amount) {
  amount = std::clamp(amount, 0.0, 1.0);
  const int64_t start = dropTick - lengthTicks;
  checkSpan(doc, start, dropTick);
  checkMacro(map, "reverb", "mix");
  RoleToolResult r;
  const RoleBinding& reverb = *map.find("reverb");
  r.keys.push_back(writeRoleSweep(doc, trackId, map, "reverb", "mix", start, dropTick, roleRest(reverb, "reverb", "mix"), 0.25 + 0.7 * amount, 2.0));
  if (hasMacro(map, "reverb", "size")) {
    const double rest = roleRest(reverb, "reverb", "size");
    r.keys.push_back(writeRoleSweep(doc, trackId, map, "reverb", "size", start, dropTick, rest, std::max(rest, 0.5 + 0.5 * amount), 1.5));
  }
  r.roles.push_back("reverb");
  if (hasMacro(map, "filter", "cutoff")) {
    const double rest = roleRest(*map.find("filter"), "filter", "cutoff");
    r.keys.push_back(writeRoleSweep(doc, trackId, map, "filter", "cutoff", start, dropTick, rest, rest * (1.0 - 0.6 * amount), 1.5));
    r.roles.push_back("filter");
  } else {
    r.skipped.push_back(whyNot(map, "filter", "cutoff"));
  }
  r.sentence = "SUCK: the reverb opens over the last " + bars(lengthTicks) + " up to " + percent(0.25 + 0.7 * amount) +
               (r.roles.size() > 1 ? ", the filter closes" : "") + ", and everything is back on the drop";
  return r;
}

RoleToolResult toolPump(Document& doc, const std::string& trackId, const std::string& sourceTrackId, const RoleMap& map,
                        int64_t start, int64_t end, double depth, int64_t releaseTicks) {
  checkSpan(doc, start, end);
  if (trackId == sourceTrackId) throw DocumentError("pump", "a track cannot pump from its own notes");
  trackById(doc, trackId);
  const Track& src = trackById(doc, sourceTrackId);
  depth = std::clamp(depth, 0.0, 1.0);
  if (releaseTicks <= 0) releaseTicks = kPpq / 2;
  releaseTicks = std::clamp<int64_t>(releaseTicks, kPpq / 16, 4 * kPpq);
  RoleToolResult r;
  r.roles.push_back("sidechain");
  const RoleBinding* b = map.find("sidechain");
  if (hasMacro(map, "sidechain", "volume")) {
    const std::vector<int64_t> onsets = songOnsets(doc, sourceTrackId, start, end);
    if (onsets.empty()) throw DocumentError("pump", "'" + src.name + "' plays no notes there");
    const double rest = roleRest(*b, "sidechain", "volume"), low = rest * (1.0 - depth);
    std::vector<AutomationPoint> points;
    for (size_t i = 0; i < onsets.size(); ++i) {
      const int64_t o = onsets[i];
      const int64_t next = i + 1 < onsets.size() ? onsets[i + 1] : end;
      points.push_back({o, low, json::object()});
      // Back up over the release, fast at first (a square root), never past the next note.
      for (int k = 1; k <= 4; ++k) {
        const int64_t t = o + releaseTicks * k / 4;
        if (t >= next || t >= end) break;
        points.push_back({t, low + (rest - low) * std::sqrt(k / 4.0), json::object()});
      }
    }
    const std::string effectId = ensureRoleEffect(doc, trackId, "sidechain", map);
    const std::string key = roleParamKey(map, "sidechain", effectId, "volume");
    setAutomationInRange(doc, trackId, key, start, end, points, rest);
    r.keys.push_back(key);
    r.sentence = "PUM: the volume dips " + percent(depth) + " at every note of " + src.name + " (" + std::to_string(onsets.size()) + " notes)";
    return r;
  }
  if (hasMacro(map, "sidechain", "depth")) {
    const std::string effectId = ensureRoleEffect(doc, trackId, "sidechain", map);
    const std::string key = roleParamKey(map, "sidechain", effectId, "depth");
    setAutomationInRange(doc, trackId, key, start, end, {{start, depth, json::object()}, {end - 1, depth, json::object()}},
                         roleRest(*b, "sidechain", "depth"));
    r.keys.push_back(key);
    r.sentence = "PUM: the sidechain plugin pumps at " + percent(depth) + " depth over " + bars(end - start);
    return r;
  }
  throw DocumentError("pump", whyNot(map, "sidechain", "volume"));
}

RoleToolResult toolBuildupFx(Document& doc, const std::string& trackId, const RoleMap& map, int64_t start, int64_t dropTick,
                             const BuildupFx& fx) {
  checkSpan(doc, start, dropTick);
  trackById(doc, trackId);
  if (!fx.any()) throw DocumentError("buildup", "choose at least one effect");
  const double a = std::clamp(fx.amount, 0.0, 1.0);
  struct Move { bool on; const char *role, *macro; double from, to, curve; bool fromRest; };
  // from < 0: the macro's rest value.
  const std::vector<Move> moves = {
      {fx.filter, "filter", "cutoff", 1.0 - 0.75 * a, 1.0, 1.5, false},
      {fx.reverb, "reverb", "mix", 0, 0.5 * a, 2.0, true},
      {fx.delay, "delay", "mix", 0, 0.5 * a, 2.0, true},
      {fx.delay, "delay", "feedback", 0, 0.35 + 0.4 * a, 1.0, true},
      {fx.drive, "drive", "amount", 0, a, 1.5, true},
  };
  // Every role asked for: mapped or skipped, decided before anything is written.
  std::vector<const Move*> todo;
  RoleToolResult r;
  for (const auto& m : moves) {
    if (!m.on) continue;
    const bool main = std::string(m.macro) != "feedback";  // the delay's feedback rides along with its mix
    if (hasMacro(map, m.role, m.macro)) todo.push_back(&m);
    else if (main) r.skipped.push_back(whyNot(map, m.role, m.macro));
  }
  if (todo.empty()) throw DocumentError("buildup", r.skipped.empty() ? "no effect to write" : r.skipped.front());
  std::vector<std::string> words;
  for (const Move* m : todo) {
    const double from = m->fromRest ? roleRest(*map.find(m->role), m->role, m->macro) : m->from;
    r.keys.push_back(writeRoleSweep(doc, trackId, map, m->role, m->macro, start, dropTick, from, m->to, m->curve));
    if (std::find(r.roles.begin(), r.roles.end(), m->role) == r.roles.end()) {
      r.roles.push_back(m->role);
      const std::string role = m->role;
      words.push_back(role == "filter" ? "the filter opens" : role == "reverb" ? "the reverb grows" : role == "delay" ? "the echoes grow" : "the drive pushes");
    }
  }
  r.sentence = "Build-up effects over " + bars(dropTick - start) + ": ";
  for (size_t i = 0; i < words.size(); ++i) r.sentence += (i ? (i + 1 == words.size() ? " and " : ", ") : "") + words[i];
  r.sentence += ", back on the drop";
  return r;
}

}  // namespace core::doc
