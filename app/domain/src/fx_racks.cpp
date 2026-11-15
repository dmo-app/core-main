#include "fx_racks.hpp"
#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include "edit.hpp"
#include "rcore.hpp"
#include "fcore.hpp"

namespace core::doc {

namespace {

[[noreturn]] void fail(const std::string& where, const std::string& message) { throw DocumentError(where, message); }

std::string text(const json& j, const char* key, const std::string& where, bool required = true, size_t max = 400) {
  if (!j.contains(key)) {
    if (required) fail(where + "." + key, "is missing");
    return {};
  }
  if (!j[key].is_string()) fail(where + "." + key, "must be text");
  const std::string s = j[key].get<std::string>();
  if (s.size() > max) fail(where + "." + key, "is too long");
  return s;
}

bool validId(const std::string& id) {
  static const std::regex re("^[a-z0-9][a-z0-9-]{0,39}(/[a-z0-9][a-z0-9-]{0,39})?$");
  return std::regex_match(id, re);
}

}  // namespace

RackDef parseRack(const json& j) {
  const std::string where = "rack";
  if (!j.is_object()) fail(where, "must be an object");
  if (j.value("format", std::string()) != kRackFormat) fail(where + ".format", "must be \"core-fx-rack\"");
  if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() < 1)
    fail(where + ".version", "must be a whole number from 1");
  RackDef d;
  d.version = j["version"].get<int>();
  d.id = text(j, "id", where, true, 81);
  if (!validId(d.id)) fail(where + ".id", "must be lower-case letters, digits and '-', with one optional '/'");
  d.name = text(j, "name", where, true, 60);
  if (d.name.empty()) fail(where + ".name", "is empty");
  d.role = text(j, "role", where, false, 60);
  d.kind = j.contains("kind") ? text(j, "kind", where) : std::string("effects");
  if (d.kind != "effects" && d.kind != "instrument") fail(where + ".kind", "must be \"effects\" or \"instrument\"");
  d.description = text(j, "description", where, false, 1000);
  d.status = text(j, "status", where, false, 40);
  auto words = [&](const char* key, std::vector<std::string>& out) {
    if (!j.contains(key)) return;
    if (!j[key].is_array() || j[key].size() > 16) fail(where + "." + key, "must be a list of at most 16");
    for (const auto& s : j[key]) {
      if (!s.is_string() || s.get<std::string>().empty() || s.get<std::string>().size() > 40) fail(where + "." + key, "must hold short words");
      out.push_back(s.get<std::string>());
    }
  };
  words("styles", d.styles);
  words("tags", d.tags);
  if (j.contains("native")) {
    if (!j["native"].is_array() || j["native"].size() > 8) fail(where + ".native", "must be a list of at most 8");
    for (size_t i = 0; i < j["native"].size(); ++i) {
      const std::string w = where + ".native[" + std::to_string(i) + "]";
      const json& x = j["native"][i];
      if (!x.is_object()) fail(w, "must be an object");
      NativeStep st{text(x, "kind", w, true, 20), text(x, "name", w, false, 60), text(x, "preset", w, false, 60)};
      if (st.kind != "effect" && st.kind != "rcore" && st.kind != "dcore" && st.kind != "instrument" && st.kind != "call")
        fail(w + ".kind", "must be effect, rcore, dcore, instrument or call");
      if ((st.kind == "effect" || st.kind == "instrument" || st.kind == "call") && st.name.empty()) fail(w + ".name", "is missing");
      if (st.kind != "call" && st.preset.empty()) fail(w + ".preset", "is missing");
      d.native.push_back(st);
    }
  }
  if (d.native.empty()) fail(where + ".native", "is empty: a Style FX needs at least one step");
  return d;
}

namespace {
const RCorePreset* findPlace(const std::string& kind, const std::string& name) {
  for (const auto& p : kind == "rcore" ? rcorePresets() : dcorePresets()) if (name == p.name) return &p;
  return nullptr;
}
bool doable(const NativeStep& s) {
  return s.kind == "effect" || s.kind == "rcore" || s.kind == "dcore" ||
         (s.kind == "call" && s.name == "fcore:build-up");
}
// The song's build-ups (sections named so), where an F-Core build-up moves on a track that is not a sampler.
std::vector<FCoreRegion> buildupRegions(const Document& doc) {
  std::vector<FCoreRegion> out;
  for (const auto& sec : doc.sections) {
    std::string n = sec.name;
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    if (n.find("build") != std::string::npos || n.find("rise") != std::string::npos) out.push_back({sec.startTick, sec.startTick + sec.lengthTicks});
  }
  return out;
}
}  // namespace

bool nativeRackReady(const RackDef& def) {
  for (const auto& s : def.native) if (doable(s)) return true;
  return false;
}

std::vector<std::string> applyNativeRack(Document& doc, const std::vector<std::string>& trackIds, const RackDef& def) {
  if (def.native.empty()) fail("rack.native", "'" + def.name + "' has no native replacement yet");
  if (trackIds.empty()) fail("rack.native", "no tracks selected");
  std::vector<std::string> later;
  // One Style FX on a track: the effects the previous one added go (with their curves) before this one's come, so
  // choosing another never stacks on the last (Diogo, 2026-11-06). The user's own effects stay.
  for (const auto& t : trackIds) {
    if (!doc.findTrack(t)) continue;
    const json& chain = effectChain(doc, t);
    for (int i = (int)chain.size() - 1; i >= 0; --i)
      if (chain[(size_t)i].is_object() && chain[(size_t)i].contains(kStyleFxKey)) removeEffect(doc, t, i);
    // The R-Core / D-Core sends a Style FX made go too (and a place nobody sends to any more).
    Track& tp = trackById(doc, t);
    if (tp.extra.is_object() && tp.extra.contains("styleFxSends") && tp.extra["styleFxSends"].is_array()) {
      const json buses = tp.extra["styleFxSends"];
      tp.extra.erase("styleFxSends");
      for (const auto& b : buses) {
        if (!b.is_string()) continue;
        const std::string bus = b.get<std::string>();
        std::vector<std::string> drop;
        for (const auto& sd : trackById(doc, t).mix.sends) if (sd.to == bus) drop.push_back(sd.id);
        for (const auto& id : drop) removeSend(doc, t, id);
        bool used = false;
        for (const auto& o : doc.tracks) {
          if (o.mix.output == bus) used = true;
          for (const auto& sd : o.mix.sends) if (sd.to == bus) used = true;
        }
        if (!used && doc.findTrack(bus)) removeTrack(doc, bus);
      }
    }
    // An F-Core build-up a Style FX put there goes with it.
    Track& tr = trackById(doc, t);
    if (tr.extra.is_object() && tr.extra.contains(kStyleFxKey)) {
      tr.extra.erase(kStyleFxKey);
      if (fcoreOf(tr)) removeFCore(doc, t);
    }
  }
  for (const auto& s : def.native) {
    if (s.kind == "effect") {
      const EffectPreset* p = findEffectPreset(s.name, s.preset);
      if (!p) fail("rack.native", "no " + s.name + " preset '" + s.preset + "'");
      for (const auto& t : trackIds) {
        const int at = addEffect(doc, t, *p);
        effectChain(doc, t)[(size_t)at][kStyleFxKey] = def.id;
      }
    } else if (s.kind == "rcore" || s.kind == "dcore") {
      const RCorePreset* p = findPlace(s.kind, s.preset);
      if (!p) fail("rack.native", "no " + s.kind + " place '" + s.preset + "'");
      const RCoreResult placed = applyRCore(doc, trackIds, *p);
      for (const auto& t : trackIds) {
        if (!doc.findTrack(t) || t == placed.busId) continue;
        Track& tr = trackById(doc, t);
        if (!tr.extra.is_object()) tr.extra = json::object();
        if (!tr.extra.contains("styleFxSends") || !tr.extra["styleFxSends"].is_array()) tr.extra["styleFxSends"] = json::array();
        tr.extra["styleFxSends"].push_back(placed.busId);
      }
    } else if (s.kind == "call" && s.name == "fcore:build-up") {
      // F-Core's build-up in the rack's style: on each note of a sampler, over the song's build-ups elsewhere.
      const std::string style = def.styles.empty() ? std::string("darkpsycore") : def.styles.front();
      applyFCoreToTracks(doc, trackIds, "buildup", style, FCoreKnobs{}, "note", buildupRegions(doc));
      for (const auto& t : trackIds) {
        Track& tr = trackById(doc, t);
        if (!tr.extra.is_object()) tr.extra = json::object();
        tr.extra[kStyleFxKey] = def.id;
      }
    } else if (s.kind == "instrument") {
      later.push_back(s.name + " \xC2\xB7 " + s.preset);
    } else {
      later.push_back(s.name);
    }
  }
  return later;
}

const std::vector<std::string>& rackStyles() {
  static const std::vector<std::string> styles = {"darkpsycore", "hitech", "slambient", "psytrance"};
  return styles;
}

std::string styleLabel(const std::string& style) {
  if (style == "darkpsycore") return "Dark psycore";
  if (style == "hitech") return "Hi-tech";
  if (style == "slambient") return "Slambient";
  if (style == "psytrance") return "Psytrance";
  return style;
}

namespace {
void readFolder(const std::filesystem::path& folder, bool builtin, std::vector<RackEntry>& out, std::set<std::string>& ids,
                std::vector<RackProblem>* problems) {
  std::error_code ec;
  if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return;
  std::vector<std::filesystem::path> files;
  for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_regular_file(ec) && name.size() > std::string(kRackExtension).size() &&
        name.compare(name.size() - std::string(kRackExtension).size(), std::string::npos, kRackExtension) == 0)
      files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    try {
      std::ifstream in(f, std::ios::binary);
      const json j = json::parse(in);
      RackDef d = parseRack(j);
      if (!ids.insert(d.id).second) {
        if (problems) problems->push_back({f, "the id '" + d.id + "' is already used by another rack" + (builtin ? "" : " (a built-in one is never replaced: save yours with a new id)")});
        continue;
      }
      out.push_back({std::move(d), f, builtin});
    } catch (const std::exception& e) {
      if (problems) problems->push_back({f, e.what()});
    }
  }
}
}  // namespace

std::vector<RackEntry> loadRacks(const std::filesystem::path& builtinFolder, const std::filesystem::path& userFolder,
                                 std::vector<RackProblem>* problems) {
  std::vector<RackEntry> out;
  std::set<std::string> ids;
  readFolder(builtinFolder, true, out, ids, problems);
  readFolder(userFolder, false, out, ids, problems);
  return out;
}

}  // namespace core::doc
