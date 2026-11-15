#include "attach.hpp"
#include "corefx_legacy.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include "edit.hpp"

namespace core::doc {

namespace {

struct FxPart {
  const char* effect;
  std::vector<std::pair<const char*, double>> values;
  const char* onParam;  // the knob that turns it on (its Mix)
  double on;            // its value where the attachment sounds
};
struct Recipe {
  AttachRecipe info;
  std::vector<FxPart> parts;
  bool sweep;  // "Open": the filter's cutoff rises over each stretch
};

const std::vector<Recipe>& recipes() {
  static const std::vector<Recipe> r = {
      {{"texture", "Texture", "A pitched echo and a slow phaser: the note gets a moving, shimmering tail"},
       {{"spiral", {{"time", 1}, {"shift", 12}, {"feedback", 0.55}}, "mix", 0.5}, {"flux", {{"mode", 0}, {"rate", 1.5}, {"depth", 0.7}, {"feedback", 0.4}}, "mix", 0.6}},
       false},
      {{"echo", "Echo", "1/8 repeats that ring on until the next note"}, {{"echo", {{"time", 1}, {"feedback", 0.45}}, "mix", 0.45}}, false},
      {{"open", "Open", "A low-pass that opens from dark to bright over the part"},
       {{"filter", {{"mode", 0}, {"cutoff", 300}, {"resonance", 3}}, "mix", 1}}, true},
      {{"comb", "Comb", "A ringing, metallic comb tuned around 1.2 kHz"}, {{"filter", {{"mode", 8}, {"cutoff", 1200}, {"resonance", 8.5}}, "mix", 0.7}}, false},
      {{"metal", "Metal", "A short ringing metal note under the sound"}, {{"metal", {{"ring", 0.6}, {"decay", 300}}, "mix", 0.55}}, false},
      {{"stutter", "Stutter up", "The part repeats in 1/16 slices an octave up"},
       {{"glitch", {{"rate", 1}, {"repeats", 4}, {"chance", 1}, {"pitch", 12}}, "mix", 1}}, false},
      {{"wash", "Wash", "A big, long space: the part dissolves into a cloud"},
       {{"space", {{"size", 0.9}, {"decay", 6}, {"predelay", 30}, {"shimmer", 0.3}}, "mix", 0.5}}, false},
      {{"grit", "Grit", "Drive and a bright edge: the part gets dirty"}, {{"saturation", {{"drive", 18}, {"output", -6}}, "mix", 1}}, false},
      // Presets (Diogo, the same day: "glitch é porreiro mas não temos tanto controlo", "podemos criar uns presets"):
      // the glitch ones always cut (Chance 100%), so they happen exactly where they are put.
      {{"stutter32", "Stutter 1/32", "Fast 1/32 repeats: a roll inside the part"},
       {{"glitch", {{"rate", 2}, {"repeats", 8}, {"chance", 1}}, "mix", 1}}, false},
      {{"stutterdown", "Stutter down", "1/16 repeats an octave down: the part falls"},
       {{"glitch", {{"rate", 1}, {"repeats", 4}, {"chance", 1}, {"pitch", -12}}, "mix", 1}}, false},
      {{"reverse", "Reverse", "The part plays backwards in 1/8 slices"},
       {{"glitch", {{"rate", 0}, {"repeats", 2}, {"chance", 1}, {"reverse", 1}}, "mix", 1}}, false},
      {{"tapestop", "Tape stop", "Short slices that drop two octaves and get shorter: a tape slowing down"},
       {{"glitch", {{"rate", 1}, {"repeats", 4}, {"chance", 1}, {"pitch", -24}, {"length", 0.5}, {"tone", -0.6}}, "mix", 1}}, false},
      {{"gate", "Gate 1/16", "The sound chopped on and off in 1/16s (a trance gate)"}, {{"pulse", {{"pattern", 1}, {"smooth", 0.15}}, "depth", 1}}, false},
      {{"phaser", "Phaser", "A slow, deep phaser sweep"}, {{"flux", {{"mode", 0}, {"rate", 0.25}, {"depth", 0.9}, {"feedback", 0.6}}, "mix", 0.7}}, false},
      {{"flanger", "Flanger jet", "A jet-plane flanger"}, {{"flux", {{"mode", 1}, {"rate", 0.15}, {"depth", 0.9}, {"feedback", 0.75}}, "mix", 0.6}}, false},
      {{"acid", "Acid", "A squelchy ladder filter, high resonance, like a 303"}, {{"filter", {{"mode", 7}, {"cutoff", 900}, {"resonance", 8}}, "mix", 1}}, false},
      {{"vowel", "Vowel", "A talking formant filter (A-E-I-O-U): the part speaks"}, {{"filter", {{"mode", 9}, {"cutoff", 700}, {"resonance", 6}}, "mix", 1}}, false},
      {{"dark", "Dark", "A low-pass at 600 Hz: the part goes muffled, far away"}, {{"filter", {{"mode", 0}, {"cutoff", 600}, {"resonance", 1}}, "mix", 1}}, false},
      {{"thin", "Thin", "A high-pass at 800 Hz: no low end, like a radio"}, {{"filter", {{"mode", 1}, {"cutoff", 800}, {"resonance", 1}}, "mix", 1}}, false},
      {{"pingpong", "Ping-pong", "1/16 echoes bouncing left and right"}, {{"echo", {{"time", 0}, {"feedback", 0.4}, {"pingpong", 1}}, "mix", 0.4}}, false},
      {{"zap", "Zap", "A laser zap on every hit"}, {{"zap", {{"amount", 0.7}, {"sweep", 2}}, "mix", 1}}, false},
  };
  return r;
}

const Recipe* findRecipe(const std::string& id) {
  for (const auto& r : recipes()) if (id == r.info.id) return &r;
  return nullptr;
}

void setExtra(json& extra, const char* key, const std::string& value) {
  if (!extra.is_object()) extra = json::object();
  if (value.empty()) extra.erase(key);
  else extra[key] = value;
}
constexpr const char* kKeys[3] = {"attachStart", "attachMiddle", "attachEnd"};
const char* placeKey(int place) { return kKeys[std::clamp(place, 0, 2)]; }
void applySides(json& extra, const AttachSides& s) {
  if (s.start) setExtra(extra, kKeys[0], *s.start);
  if (s.middle) setExtra(extra, kKeys[1], *s.middle);
  if (s.end) setExtra(extra, kKeys[2], *s.end);
}

bool isAttach(const json& fx) { return fx.is_object() && fx.contains("attach") && fx["attach"].is_object(); }
std::string attachName(const json& fx) {
  if (!isAttach(fx)) return {};
  const json& a = fx["attach"];
  return a.contains("name") && a["name"].is_string() ? a["name"].get<std::string>() : std::string();
}
std::string extraString(const json& extra, const char* key) {
  return extra.is_object() && extra.contains(key) && extra[key].is_string() ? extra[key].get<std::string>() : std::string();
}

std::vector<std::pair<int64_t, int64_t>> joinSpans(std::vector<std::pair<int64_t, int64_t>> spans) {
  std::sort(spans.begin(), spans.end());
  std::vector<std::pair<int64_t, int64_t>> joined;
  for (const auto& s : spans) {
    if (!joined.empty() && s.first <= joined.back().second + 2 * kAttachRamp) joined.back().second = std::max(joined.back().second, s.second);
    else joined.push_back(s);
  }
  return joined;
}

bool trackHasAttaches(const Document& doc, const Track& t) {
  try {
    for (const auto& fx : effectChain(doc, t.id)) if (isAttach(fx)) return true;
  } catch (...) {}
  return false;
}

}  // namespace

const std::vector<AttachRecipe>& attachRecipes() {
  static const std::vector<AttachRecipe> list = [] {
    std::vector<AttachRecipe> v;
    for (const auto& r : recipes()) v.push_back(r.info);
    return v;
  }();
  return list;
}

std::vector<AttachInfo> trackAttaches(const Document& doc, const std::string& trackId) {
  std::vector<AttachInfo> out;
  for (const auto& fx : effectChain(doc, trackId)) {
    const std::string name = attachName(fx);
    if (name.empty()) continue;
    bool seen = false;
    for (const auto& a : out) seen = seen || a.name == name;
    if (!seen) out.push_back({name, extraString(fx["attach"], "recipe")});
  }
  return out;
}

namespace {
std::vector<json>& userList() { static std::vector<json> list; return list; }
const json* findUser(const std::string& recipe) {
  if (recipe.rfind("user:", 0) != 0) return nullptr;
  const std::string name = recipe.substr(5);
  for (const auto& u : userList()) if (u.value("name", std::string()) == name) return &u;
  return nullptr;
}
// An old effect's switch (its Mix) is Core FX's Amount, at the same level.
// The part's values and the knob that switches it: on a Core FX made from an old effect, its module's On (the
// tail rings out when the part ends) with Amount at the old Mix; on any other effect, its own knob from 0.
std::pair<std::string, double> attachSwitch(const std::string& effect, const std::string& param, double on, json& values) {
  if (isLegacyEffect(effect) && param == "mix") return coreFxAttachSwitch(effect, on, values);
  values[param] = 0;
  return {param, on};
}
// The knob that turns an effect on, in the order looked for.
const char* onParamOf(const EffectInfo& info) {
  for (const char* want : {"mix", "depth", "amount"})
    for (const auto& p : info.params) if (want == std::string(p.name)) return want;
  return nullptr;
}
}  // namespace

void setUserAttachments(std::vector<json> list) { userList() = std::move(list); }
const std::vector<json>& userAttachments() { return userList(); }

json attachmentFromEffect(const json& fx, const std::string& wanted) {
  const std::string effect = effectName(fx);
  const EffectInfo* info = findEffect(effect);
  if (!info) throw DocumentError("attachments", "only Core's own effects can be saved as an attachment");
  const char* on = onParamOf(*info);
  if (!on) throw DocumentError("attachments", std::string(info->label) + " has no Mix: it cannot be turned on and off by an attachment");
  std::string name = wanted.substr(0, 80);  // "Folder/Name" fits
  while (!name.empty() && name.back() == ' ') name.pop_back();
  if (name.empty()) throw DocumentError("attachments", "a name cannot be empty");
  json params = json::object();
  for (const auto& p : info->params) params[p.name] = effectParam(fx, p.name, p.def);
  double level = params[on].get<double>();
  if (level <= 0) level = 1;  // saved while silent: it sounds fully where it is put
  params.erase(on);
  return {{"name", name}, {"effects", json::array({{{"effect", effect}, {"params", params}, {"param", on}, {"on", level}}})}};
}

namespace {
// A file or folder name from a user's words: letters, digits, - and _ (and any non-ASCII letter); the rest becomes _.
std::string safeLeaf(const std::string& words, const char* empty) {
  std::string out;
  for (char c : words) out += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80) ? c : '_';
  return out.empty() ? std::string(empty) : out;
}
std::string trimmed(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.back() == ' ')) s.erase(s.front() == ' ' ? s.begin() : s.end() - 1);
  return s;
}
}  // namespace

std::vector<json> loadUserAttachments(const std::filesystem::path& folder) {
  std::vector<json> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return out;
  for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    const auto& entry = *it;
    if (it.depth() > 3) it.disable_recursion_pending();  // folders in folders, a few deep
    if (entry.path().extension() != ".json") continue;
    try {
      std::ifstream in(entry.path(), std::ios::binary);
      json j = json::parse(in);
      if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || !j.contains("effects") || !j["effects"].is_array() ||
          j["effects"].empty())
        continue;
      bool ok = true;
      for (const auto& e : j["effects"])
        ok = ok && e.is_object() && e.contains("effect") && e["effect"].is_string() && findEffect(e["effect"].get<std::string>()) &&
             e.contains("param") && e["param"].is_string();
      if (!ok) continue;
      // The folder it is in, from where it lies (not from the file): "Glitch", "Glitch/Mine"; none at the top.
      const auto rel = entry.path().parent_path().lexically_relative(folder);
      const auto u = rel.generic_u8string();
      const std::string where(u.begin(), u.end());
      if (where.empty() || where == ".") j.erase("folder");
      else j["folder"] = where;
      out.push_back(std::move(j));
    } catch (...) {
    }
  }
  std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
    const std::string fa = a.value("folder", std::string()), fb = b.value("folder", std::string());
    return fa != fb ? fa < fb : a["name"].get<std::string>() < b["name"].get<std::string>();
  });
  return out;
}

std::filesystem::path saveUserAttachment(const std::filesystem::path& folder, const json& attachment) {
  // "Glitch/My roll": the folder Glitch, the name My roll (the library is kept in folders, as the user likes).
  std::string name = trimmed(attachment.value("name", std::string("attachment")));
  std::string where = attachment.value("folder", std::string());
  if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) {
    where += (where.empty() ? "" : "/") + name.substr(0, slash);
    name = trimmed(name.substr(slash + 1));
  }
  if (name.empty()) throw DocumentError("attachments", "an attachment needs a name");
  std::filesystem::path dir = folder;
  std::string part;
  for (size_t i = 0; i <= where.size(); ++i) {
    if (i == where.size() || where[i] == '/' || where[i] == '\\') {
      part = trimmed(part);
      if (!part.empty() && part != "." && part != "..") {
        const std::string leaf = safeLeaf(part, "folder");
        dir /= std::filesystem::path(std::u8string(leaf.begin(), leaf.end()));
      }
      part.clear();
    } else {
      part += where[i];
    }
  }
  json stored = attachment;
  stored["name"] = name;
  stored.erase("folder");  // the folder is where the file lies
  std::filesystem::create_directories(dir);
  const std::string leaf = safeLeaf(name, "attachment") + ".json";
  const auto path = dir / std::filesystem::path(std::u8string(leaf.begin(), leaf.end()));
  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << stored.dump(2);
    if (!out) throw DocumentError("attachments", "could not write " + leaf);
  }
  std::filesystem::rename(tmp, path);
  return path;
}

const char* attachRecipeFolder(const std::string& id) {
  static const std::vector<std::pair<const char*, std::vector<std::string>>> folders = {
      {"Glitch", {"stutter", "stutter32", "stutterdown", "reverse", "tapestop", "gate", "zap"}},
      {"Filter", {"open", "acid", "vowel", "dark", "thin", "comb"}},
      {"Space", {"echo", "pingpong", "wash"}},
      {"Movement", {"texture", "phaser", "flanger"}},
      {"Colour", {"metal", "grit"}},
  };
  for (const auto& [folder, ids] : folders)
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) return folder;
  return "Other";
}

std::string addAttach(Document& doc, const std::string& trackId, const std::string& recipe, const std::string& wanted) {
  const Recipe* r = findRecipe(recipe);
  const json* user = r ? nullptr : findUser(recipe);
  if (!r && !user) throw DocumentError("attachments", "unknown attachment '" + recipe + "'");
  json& chain = effectChain(doc, trackId);
  const size_t parts = r ? r->parts.size() : (*user)["effects"].size();
  if ((int)(chain.size() + parts) > kMaxEffectsPerChain)
    throw DocumentError("attachments", "the track has no room for more effects (16 at most)");
  std::string base = wanted.empty() ? (r ? std::string(r->info.name) : (*user)["name"].get<std::string>()) : wanted.substr(0, 40), name = base;
  const auto taken = [&](const std::string& n) {
    for (const auto& a : trackAttaches(doc, trackId)) if (a.name == n) return true;
    return false;
  };
  for (int k = 2; taken(name); ++k) name = base + " " + std::to_string(k);
  if (r) {
    for (const auto& part : r->parts) {
      json values = json::object();
      for (const auto& [param, value] : part.values) values[param] = value;
      values[part.onParam] = 0;
      json switched = json::object();
      auto [on, level] = attachSwitch(part.effect, part.onParam, part.on, switched);
      const int at = addEffectWith(doc, trackId, part.effect, values);
      for (const auto& [k, v] : switched.items()) setEffectParam(doc, trackId, at, k, v.get<double>());
      effectChain(doc, trackId)[(size_t)at]["attach"] = {{"name", name}, {"recipe", recipe}, {"param", on}, {"on", level}};
    }
    return name;
  }
  for (const auto& part : (*user)["effects"]) {
    const std::string effect = part["effect"].get<std::string>();
    json values = json::object();
    if (const EffectInfo* info = findEffect(isLegacyEffect(effect) ? std::string() : effect); info && part.contains("params") && part["params"].is_object()) {
      for (const auto& p : info->params)
        if (part["params"].contains(p.name) && part["params"][p.name].is_number()) values[p.name] = std::clamp(part["params"][p.name].get<double>(), p.min, p.max);
    } else if (part.contains("params") && part["params"].is_object()) {
      values = part["params"];  // an attachment saved from an old effect: Core FX takes its values
    }
    const std::string onParam = part["param"].get<std::string>();
    values[onParam] = 0;
    const double given = part.contains("on") && part["on"].is_number() ? part["on"].get<double>() : 1.0;
    json switched = json::object();
    auto [on, level] = attachSwitch(effect, onParam, given, switched);
    const int at = addEffectWith(doc, trackId, effect, values);
    for (const auto& [k, v] : switched.items()) setEffectParam(doc, trackId, at, k, v.get<double>());
    effectChain(doc, trackId)[(size_t)at]["attach"] = {{"name", name}, {"recipe", recipe}, {"param", on}, {"on", level}};
  }
  return name;
}

void renameAttach(Document& doc, const std::string& trackId, const std::string& from, const std::string& wanted) {
  const std::string to = wanted.substr(0, 40);
  if (to.empty()) throw DocumentError("attachments", "a name cannot be empty");
  if (to == from) return;
  for (const auto& a : trackAttaches(doc, trackId)) if (a.name == to) throw DocumentError("attachments", "'" + to + "' is taken");
  for (auto& fx : effectChain(doc, trackId)) if (attachName(fx) == from) fx["attach"]["name"] = to;
  Track& t = trackById(doc, trackId);
  for (const char* key : kKeys) {
    for (auto& p : t.patterns) for (auto& n : p.notes) if (extraString(n.extra, key) == from) n.extra[key] = to;
    for (auto& c : t.clips) if (extraString(c.extra, key) == from) c.extra[key] = to;
  }
}

void removeAttach(Document& doc, const std::string& trackId, const std::string& name) {
  for (;;) {
    const json& chain = effectChain(doc, trackId);
    int at = -1;
    for (size_t i = 0; i < chain.size() && at < 0; ++i) if (attachName(chain[i]) == name) at = (int)i;
    if (at < 0) break;
    removeEffect(doc, trackId, at);  // its curves go with it
  }
  Track& t = trackById(doc, trackId);
  for (const char* key : kKeys) {
    for (auto& p : t.patterns) for (auto& n : p.notes) if (extraString(n.extra, key) == name) n.extra.erase(key);
    for (auto& c : t.clips) if (extraString(c.extra, key) == name) c.extra.erase(key);
  }
}

std::string noteAttach(const Note& n, int place) { return extraString(n.extra, placeKey(place)); }
std::string clipAttach(const Clip& c, int place) { return extraString(c.extra, placeKey(place)); }
int64_t clipAttachEdge(const Clip& c) {
  int64_t edge = 4 * kPpq;
  if (c.extra.is_object() && c.extra.contains("attachEdge") && c.extra["attachEdge"].is_number_integer()) edge = c.extra["attachEdge"].get<int64_t>();
  return std::clamp<int64_t>(edge, kPpq / 4, std::max<int64_t>(kPpq / 4, c.lengthTicks / 2));
}

int attachNotes(Pattern& p, const std::set<std::string>& ids, const AttachSides& s) {
  int changed = 0;
  for (auto& n : p.notes) {
    if (!ids.count(n.id)) continue;
    const json before = n.extra;
    applySides(n.extra, s);
    if (n.extra != before) ++changed;
  }
  return changed;
}

void attachClip(Track& t, const std::string& clipId, const AttachSides& s, int64_t edgeTicks) {
  for (auto& c : t.clips) {
    if (c.id != clipId) continue;
    applySides(c.extra, s);
    if (edgeTicks > 0) c.extra["attachEdge"] = edgeTicks;
    return;
  }
  throw DocumentError("attachments", "no clip '" + clipId + "'");
}

std::vector<std::pair<int64_t, int64_t>> attachSpans(const Document& doc, const std::string& trackId, const std::string& name) {
  std::vector<std::pair<int64_t, int64_t>> spans;
  const Track* t = doc.findTrack(trackId);
  if (!t || name.empty()) return spans;
  const auto add = [&](int64_t a, int64_t b, int64_t lo, int64_t hi) {
    a = std::max(a, lo), b = std::min(b, hi);
    if (b > a) spans.push_back({a, b});
  };
  for (const auto& c : t->clips) {
    const int64_t clipEnd = c.startTick + c.lengthTicks;
    if (c.lengthTicks <= 0) continue;
    const int64_t edge = clipAttachEdge(c);
    if (clipAttach(c, kAttachStart) == name) add(c.startTick, c.startTick + edge, c.startTick, clipEnd);
    if (clipAttach(c, kAttachMiddle) == name) add(c.startTick, clipEnd, c.startTick, clipEnd);
    if (clipAttach(c, kAttachEnd) == name) add(clipEnd - edge, clipEnd, c.startTick, clipEnd);
    const Pattern* p = nullptr;
    for (const auto& q : t->patterns) if (q.id == c.patternId) p = &q;
    if (!p || p->lengthTicks <= 0) continue;
    std::vector<const Note*> notes;
    for (const auto& n : p->notes)
      if (n.tick >= 0 && n.tick < p->lengthTicks && (noteAttach(n, kAttachStart) == name || noteAttach(n, kAttachMiddle) == name || noteAttach(n, kAttachEnd) == name)) notes.push_back(&n);
    if (notes.empty()) continue;
    std::vector<int64_t> starts;
    for (const auto& n : p->notes) starts.push_back(n.tick);
    std::sort(starts.begin(), starts.end());
    for (int64_t base = c.startTick - c.offsetTicks; base < clipEnd; base += p->lengthTicks) {
      if (base + p->lengthTicks <= c.startTick) continue;
      for (const Note* n : notes) {
        const int64_t len = std::max<int64_t>(3, n->length);
        const int64_t a = n->tick + len / 3, b = n->tick + 2 * len / 3;
        const auto next = std::upper_bound(starts.begin(), starts.end(), n->tick);
        const int64_t until = next != starts.end() ? *next : p->lengthTicks + (starts.empty() ? 0 : starts.front());
        if (noteAttach(*n, kAttachStart) == name) add(base + n->tick, base + a, c.startTick, clipEnd);
        if (noteAttach(*n, kAttachMiddle) == name) add(base + n->tick, base + n->tick + len, c.startTick, clipEnd);
        if (noteAttach(*n, kAttachEnd) == name) add(base + b, base + std::max(until, b + 1), c.startTick, clipEnd);
      }
    }
  }
  return joinSpans(std::move(spans));
}

std::vector<AttachSides> planAttachments(size_t count, const AttachPlan& plan) {
  std::vector<AttachSides> out(count);
  const auto& names = plan.names;
  if (names.empty()) return out;
  const size_t n = names.size();
  uint64_t state = plan.seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
  const auto draw = [&]() -> std::string {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    if (z % 4 == 0) return std::string();  // one draw in four: nothing there
    return names[(size_t)((z >> 2) % n)];
  };
  for (size_t i = 0; i < count; ++i) {
    std::string start, middle = names[i % n], end;
    if (plan.mode == "random") {
      start = draw(), middle = draw(), end = draw();
    } else if (plan.mode == "alternate") {
      if (n == 1) start = end = (i % 2 == 0) ? names[0] : std::string();
      else start = end = names[i % n];
    } else {  // same: link i (end of i, start of i + 1) is names[i % n]
      end = names[i % n];
      start = names[(i + n - 1) % n];
    }
    if (plan.start) out[i].start = start;
    if (plan.middle) out[i].middle = middle;
    if (plan.end) out[i].end = end;
  }
  return out;
}

int applyAttachPlan(Pattern& p, const std::set<std::string>& ids, const AttachPlan& plan) {
  std::vector<Note*> notes;
  for (auto& n : p.notes) if (ids.count(n.id)) notes.push_back(&n);
  std::stable_sort(notes.begin(), notes.end(), [](const Note* a, const Note* b) { return a->tick < b->tick; });
  const auto sides = planAttachments(notes.size(), plan);
  int changed = 0;
  for (size_t i = 0; i < notes.size(); ++i) {
    const json before = notes[i]->extra;
    applySides(notes[i]->extra, sides[i]);
    if (notes[i]->extra != before) ++changed;
  }
  return changed;
}

int applyAttachPlan(Track& t, const std::vector<std::string>& clipIds, const AttachPlan& plan) {
  std::vector<Clip*> clips;
  for (auto& c : t.clips) if (std::find(clipIds.begin(), clipIds.end(), c.id) != clipIds.end()) clips.push_back(&c);
  std::stable_sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) { return a->startTick < b->startTick; });
  const auto sides = planAttachments(clips.size(), plan);
  int changed = 0;
  for (size_t i = 0; i < clips.size(); ++i) {
    const json before = clips[i]->extra;
    applySides(clips[i]->extra, sides[i]);
    if (clips[i]->extra != before) ++changed;
  }
  return changed;
}

std::string ensureAttach(Document& doc, const std::string& trackId, const std::string& recipe) {
  for (const auto& a : trackAttaches(doc, trackId)) if (a.recipe == recipe) return a.name;
  return addAttach(doc, trackId, recipe);
}

void refreshAttaches(Document& doc) {
  for (const auto& track : doc.tracks) {
    if (!trackHasAttaches(doc, track)) continue;
    const std::string trackId = track.id;
    try {
      std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> cache;
      const json chain = effectChain(doc, trackId);
      for (size_t i = 0; i < chain.size(); ++i) {
        const std::string name = attachName(chain[i]);
        if (name.empty() || !chain[i].contains("id") || !chain[i]["id"].is_string()) continue;
        if (!cache.count(name)) cache[name] = attachSpans(doc, trackId, name);
        const auto& spans = cache[name];
        const json& a = chain[i]["attach"];
        const std::string param = extraString(a, "param").empty() ? "mix" : extraString(a, "param");
        const double on = a.contains("on") && a["on"].is_number() ? a["on"].get<double>() : 1.0;
        const std::string id = chain[i]["id"].get<std::string>();
        const std::string key = effectParamKey(id, param);
        clearAutomation(doc, trackId, key);
        const Recipe* r = findRecipe(extraString(a, "recipe"));
        const bool sweep = r && r->sweep && effectName(chain[i]) == "filter";
        const std::string cutKey = effectParamKey(id, "cutoff");
        if (sweep) clearAutomation(doc, trackId, cutKey);
        setEffectParam(doc, trackId, (int)i, param, 0);
        // On Core FX the switch is a module's On: when a stretch ends the module stops receiving and its tail rings out.
        if (spans.empty()) continue;
        const auto curve = [&](const std::string& k, const std::vector<std::pair<int64_t, int64_t>>& where, double level) {
          setAutomationPoint(doc, trackId, k, 0, 0);
          for (const auto& [from, to] : where) {
            const int64_t in = std::max<int64_t>(0, from - kAttachRamp);
            if (in > 0) setAutomationPoint(doc, trackId, k, in, 0);
            setAutomationPoint(doc, trackId, k, from, level);
            setAutomationPoint(doc, trackId, k, to, level);
            setAutomationPoint(doc, trackId, k, to + kAttachRamp, 0);
          }
        };
        curve(key, spans, on);
        for (const auto& [from, to] : spans) {
          if (sweep) {
            setAutomationPoint(doc, trackId, cutKey, from, 300);
            setAutomationPoint(doc, trackId, cutKey, std::max(from + 1, to), 8000);
            setAutomationPoint(doc, trackId, cutKey, to + kAttachRamp + 1, 300);
          }
        }
      }
    } catch (...) {
      // A broken effect or a full curve never stops an edit: that track's attachments keep their last curves.
    }
  }
}

}  // namespace core::doc
