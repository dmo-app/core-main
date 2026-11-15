#include "rcore.hpp"
#include <set>
#include <algorithm>
#include <cctype>
#include "edit.hpp"

namespace core::doc {

// Send levels: the style racks it replaces ran the reverb at 15-35 % mix on the track (about -16 to -9 dB of
// tail); the AI guide (AICORE docs/knowledge/sound-techniques) keeps drops drier and lets breaks open up.
const std::vector<RCorePreset>& rcorePresets() {
  static const std::vector<RCorePreset> presets = {
      {"R-Core", "space", "darkpsycore", "Dark room", "Dark room", "A short dark room shared by the bass top, hats and FX; drier in the drops", -12, -4, 0},
      {"R-Core", "space", "darkpsycore", "Night hall", "Night hall", "A dark hall behind pads and FX; it opens up in the breaks", -14, -6, 0},
      {"R-Core", "space", "psytrance", "Progressive space", "Progressive space", "A soft 2 s hall for leads and pads; a little less in the drops", -12, -4, 0},
      {"R-Core", "space", "hitech", "Tight room", "Tight room", "Hi-tech stays dry: a short room only to glue, almost gone in the drops", -16, -6, 0},
      {"R-Core", "space", "slambient", "Abyss", "Abyss", "A huge 7 s room for slow hits, the same all song long", -10, 0, 0},
      {"R-Core", "space", "slambient", "Huge space", "Huge space", "Endless space for the whole slambient background", -8, 0, 0},
      {"R-Core", "space", "slambient", "Haunted", "Haunted", "A long wobbling tail, a little held back when the beat is in", -10, -2, 0},
      // The reverb types (2026-11-07): a plate for claps and snares, a spring for dub FX, a cathedral, a reverse swell.
      {"R-Core", "space", "psytrance", "Psy clap plate", "Psy clap plate", "A bright plate shared by claps, snares and vocals; drier in the drops", -12, -4, 0},
      {"R-Core", "space", "darkpsycore", "Rusty spring", "Rusty spring", "A twangy spring for FX and stabs; louder in the breaks", -12, -4, 0},
      {"R-Core", "space", "slambient", "Cathedral", "Cathedral", "A huge stone church behind pads and slow hits, the same all song long", -10, 0, 0},
      {"R-Core", "space", "slambient", "Reverse swell", "Reverse swell", "Every hit swells up after itself like a ghost; held back when the beat is in", -10, -3, 0}};
  return presets;
}

// D-Core: the style racks ran their delays at 20-25 % mix (about -12 dB of echoes); drops keep the echoes down
// so the bass groove stays clear, breaks and build-ups let them out (AI guide, sound-techniques).
const std::vector<RCorePreset>& dcorePresets() {
  static const std::vector<RCorePreset> presets = {
      {"D-Core", "echo", "psytrance", "Acid 3/16", "Acid 3/16", "Dotted-eighth echoes shared by the leads, ducked under the notes", -12, -3, 0},
      {"D-Core", "echo", "psytrance", "Progressive 1/8", "Progressive 1/8", "Soft dark eighths behind pads and leads", -14, -3, 0},
      {"D-Core", "echo", "hitech", "Motion 1/16", "Motion 1/16", "Fast sixteenths for leads and FX, held down in the drops", -14, -6, 0},
      {"D-Core", "echo", "darkpsycore", "Dark spiral", "Dark trail", "A long dark trail around the leads, louder in the breaks", -12, -4, 0},
      {"D-Core", "echo", "darkpsycore", "Throw 1/4", "Throw 1/4", "Quarter-note throws for vocals and FX at the end of phrases", -10, 0, 0},
      {"D-Core", "echo", "slambient", "Slow canyon 1/2", "Slow canyon 1/2", "Half-note echoes far away, the same all song long", -10, 0, 0}};
  return presets;
}

namespace {
std::string busName(const RCorePreset& p) { return std::string(p.gesture) + " \xC2\xB7 " + p.name; }
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// Which place a bus is: its owner tag (gesture + place name), so renaming the bus keeps it the same place (audit
// 2026-11-07, 10). Buses from before the tag are known by their name, and get the tag when used again.
constexpr const char* kPlaceKey = "corePlace";
bool tagged(const Track& t) { return t.extra.contains(kPlaceKey) && t.extra[kPlaceKey].is_object(); }
bool isPlaceOf(const Track& t, const std::string& gesture) {
  if (t.instrument.kind != "bus") return false;
  if (tagged(t)) return t.extra[kPlaceKey].value("gesture", "") == gesture;
  return t.name.rfind(gesture + " \xC2\xB7 ", 0) == 0;
}
bool isPlace(const Track& t, const RCorePreset& p) {
  if (!isPlaceOf(t, p.gesture)) return false;
  if (tagged(t)) return t.extra[kPlaceKey].value("place", "") == p.name;
  return t.name == busName(p);
}
const EffectPreset* spacePreset(const RCorePreset& p) {
  return findEffectPreset(p.effect, p.space);
}
}  // namespace

std::string rcoreBus(const Document& doc, const RCorePreset& preset) {
  for (const auto& t : doc.tracks)  // a tagged bus first: an untagged one may only share the name
    if (tagged(t) && isPlace(t, preset)) return t.id;
  for (const auto& t : doc.tracks)
    if (!tagged(t) && isPlace(t, preset)) return t.id;
  return "";
}

RCoreResult applyRCore(Document& doc, const std::vector<std::string>& trackIds, const RCorePreset& preset, bool bySection) {
  if (trackIds.empty()) throw DocumentError("rcore", "no tracks selected");
  const EffectPreset* space = spacePreset(preset);
  if (!space) throw DocumentError("rcore", std::string("no ") + preset.effect + " preset '" + preset.space + "'");
  RCoreResult out;
  out.busId = rcoreBus(doc, preset);
  if (out.busId.empty()) {
    out.busId = addBusTrack(doc, busName(preset));
    out.newBus = true;
    const int at = addEffect(doc, out.busId, *space);
    setEffectParam(doc, out.busId, at, "amount", 1.0);  // only the tail: the dry sound stays on each track
    setEffectParam(doc, out.busId, at, "fxOnly", 1.0);
  }
  trackById(doc, out.busId).extra[kPlaceKey] = {{"gesture", preset.gesture}, {"place", preset.name}};
  // One place per gesture on a track: choosing another R-Core place moves the send there (two reverbs would stack
  // on each other, Diogo 2026-11-06). A place no track sends to any more goes too.
  std::set<std::string> left;
  for (const auto& id : trackIds) {
    if (id == out.busId || !doc.findTrack(id)) continue;
    const Track& t = trackById(doc, id);
    std::vector<std::string> drop;
    for (const auto& s : t.mix.sends) {
      const Track* to = doc.findTrack(s.to);
      if (s.to != out.busId && to && isPlaceOf(*to, preset.gesture)) { drop.push_back(s.id); left.insert(s.to); }
    }
    for (const auto& sid : drop) removeSend(doc, id, sid);
  }
  for (const auto& bus : left) {
    bool used = false;
    for (const auto& t : doc.tracks) {
      if (t.mix.output == bus) used = true;
      for (const auto& s : t.mix.sends) if (s.to == bus) used = true;
    }
    if (!used && doc.findTrack(bus)) removeTrack(doc, bus);
  }
  for (const auto& id : trackIds) {
    if (id == out.busId || !doc.findTrack(id)) continue;
    try {
      Track& t = trackById(doc, id);
      std::string sendId;
      for (const auto& s : t.mix.sends) if (s.to == out.busId) sendId = s.id;
      if (sendId.empty()) addSend(doc, id, out.busId, preset.sendDb, false);
      else setSend(doc, id, sendId, preset.sendDb, false);
      ++out.sends;
    } catch (const DocumentError&) {
      // A track that cannot feed this bus (it is routed through it, a loop): left as it was.
    }
  }
  if (bySection && (preset.dropDb != 0 || preset.breakDb != 0)) {
    for (const auto& s : doc.sections) {
      const std::string n = lower(s.name);
      double db = 0;
      if (n.find("drop") != std::string::npos) db = preset.dropDb;
      else if (n.find("break") != std::string::npos || n.find("intro") != std::string::npos || n.find("build") != std::string::npos ||
               n.find("outro") != std::string::npos)
        db = preset.breakDb;
      const int64_t end = s.startTick + std::max<int64_t>(1, s.lengthTicks) - 1;
      setAutomationPoint(doc, out.busId, "gainDb", s.startTick, db);
      setAutomationPoint(doc, out.busId, "gainDb", end, db);
      ++out.sections;
    }
  }
  return out;
}

}  // namespace core::doc
