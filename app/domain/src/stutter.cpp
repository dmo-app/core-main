#include "stutter.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include "edit.hpp"

namespace core::doc {

namespace {
// What a tool leaves on the notes and clips it made (the Core plugin catalogue that stamped a version went with Core's
// own sound, 2026-11-16): the state as it is.
json corePluginState(const std::string&, json state) { return state; }
}  // namespace

const std::vector<StutterPreset>& stutterPresets() {
  // Starting points from the plan's walkthroughs (hi-tech accelerating roll 1/8 -> 1/64, "three repeats, then the
  // whole word") and the classic trance/psy gate and stammer figures. Hypotheses until heard.
  constexpr int64_t q = kPpq;
  static const std::vector<StutterPreset> list = {
      {"accel-roll", "Accelerating Roll", "hitech", "Repeats that speed up from 1/8 to 1/64 across the note, getting louder.",
       {q / 2, q / 16, 0.6, 0.5, 1.0, 0, 0, false}},
      {"gate-16", "Gate 1/16", "psytrance", "The note chopped into even 1/16 pulses: the trance gate on a pad or lead.",
       {q / 4, q / 4, 0.5, 1.0, 1.0, 0, 0, false}},
      {"triplet-stammer", "Triplet Stammer", "darkpsycore", "1/16 triplets that fade away: a nervous, off-grid stammer.",
       {q / 6, q / 6, 0.55, 1.0, 0.45, 0, 0, false}},
      {"falling-stairs", "Falling Stairs", "darkpsycore", "1/32 repeats that step down a semitone each time: a dark, sinking roll.",
       {q / 8, q / 8, 0.7, 1.0, 0.6, -1, 0, false}},
      {"three-then-whole", "Three, Then Whole", "psytrance", "Three short 1/16 hits, then the note itself plays to its end.",
       {q / 4, q / 4, 0.45, 0.8, 1.0, 0, 3, true}},
      {"slow-to-fast", "Slow to Fast", "slambient", "From quarter notes to 1/32 over the note: a long build into the drop.",
       {q, q / 8, 0.5, 0.35, 1.0, 0, 0, false}},
      {"machine-128", "Machine 1/128", "hitech", "A buzzing 1/128 stutter: so fast it becomes a tone.",
       {q / 32, q / 32, 0.6, 0.9, 0.9, 0, 0, false}},
  };
  return list;
}

const StutterPreset* findStutterPreset(const std::string& id) {
  for (const auto& p : stutterPresets()) if (id == p.id) return &p;
  return nullptr;
}

std::vector<int64_t> stutterOnsets(const StutterRecipe& r, int64_t length) {
  std::vector<int64_t> out;
  if (length <= 0 || r.gapFrom < 1 || r.gapTo < 1) return out;
  const double from = (double)r.gapFrom, to = (double)r.gapTo;
  double t = 0;  // exact position, rounded once per onset
  while (true) {
    const int64_t at = (int64_t)std::llround(t);
    if (at >= length) break;
    if (out.empty() || at > out.back()) out.push_back(at);
    if (r.count > 0 && (int)out.size() >= r.count) break;
    if (out.size() > 16384) break;  // the plan's per-instance ceiling; never reached by a sane recipe
    const double u = std::clamp(t / (double)length, 0.0, 1.0);
    t += from * std::pow(to / from, u);
  }
  return out;
}

int stutterNotes(Pattern& pattern, const std::set<std::string>& ids, const StutterRecipe& r, const std::string& presetId, bool pitched) {
  std::vector<Note> keep, made;
  int written = 0;
  for (const auto& n : pattern.notes) {
    if (!ids.count(n.id)) { keep.push_back(n); continue; }
    const auto onsets = stutterOnsets(r, n.length);
    // the whole note after the repeats, from the next onset to the note's end (only when there is room for it)
    const int64_t exitAt = onsets.empty() ? n.length : onsets.back() + std::max<int64_t>(1, r.gapTo);
    const bool exitNote = r.finalNote && r.count > 0 && exitAt < n.length;
    if (onsets.size() + (exitNote ? 1 : 0) < 2) { keep.push_back(n); continue; }  // too short to stutter: left alone
    const size_t total = onsets.size();
    for (size_t k = 0; k < total; ++k) {
      const int64_t start = onsets[k];
      const int64_t next = k + 1 < total ? onsets[k + 1] : n.length;
      // the gap this repeat owns (the last one: the recipe's gap at the end, inside the note)
      const double u = (double)start / (double)n.length;
      const int64_t gap = k + 1 < total ? next - start
                                        : std::min<int64_t>(n.length - start,
                                                            (int64_t)std::llround((double)r.gapFrom * std::pow((double)r.gapTo / (double)r.gapFrom, u)));
      Note x = n;
      x.tick = n.tick + start;
      x.length = std::max<int64_t>(1, std::min<int64_t>(n.length - start, (int64_t)std::llround(gap * std::clamp(r.gate, 0.05, 1.0))));
      const double f = total > 1 ? (double)k / (double)(total - 1) : 0.0;
      x.velocity = std::clamp(n.velocity * (r.velocityFrom + (r.velocityTo - r.velocityFrom) * f), 0.01, 1.0);
      if (pitched) x.pitch = std::clamp(n.pitch + r.pitchStep * (int)k, 0, 127);
      x.hasRoll = false; x.roll = Roll{};
      x.extra["stutter"] = corePluginState("stutter", {{"preset", presetId}, {"of", n.id}, {"index", (int)k}, {"generator", 1}});
      made.push_back(std::move(x));
    }
    if (exitNote) {
      const int64_t start = exitAt;
      Note x = n;
      x.tick = n.tick + start;
      x.length = n.length - start;
      x.hasRoll = false; x.roll = Roll{};
      x.extra["stutter"] = corePluginState("stutter", {{"preset", presetId}, {"of", n.id}, {"index", (int)total}, {"exit", "final-note"}, {"generator", 1}});
      made.push_back(std::move(x));
    }
    written += (int)total + (exitNote ? 1 : 0);
  }
  if (written == 0) return 0;
  // More notes than a pattern may hold: nothing is written (the song would save, then never open again: audit
  // 2026-11-07, machine-128 on a 64-bar note made 8192).
  if (keep.size() + made.size() > kMaxPatternNotes)
    throw DocumentError("stutter", "this would make " + std::to_string(keep.size() + made.size()) + " notes in one pattern (at most " +
                                       std::to_string(kMaxPatternNotes) + "): choose fewer or shorter notes");
  pattern.notes = std::move(keep);
  for (auto& x : made) { x.id = newNoteId(pattern); pattern.notes.push_back(std::move(x)); }
  std::sort(pattern.notes.begin(), pattern.notes.end(),
            [](const Note& a, const Note& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
  return written;
}

int stutterTrack(Document& doc, const std::string& trackId, int64_t from, int64_t to, const StutterPreset& preset) {
  Track& t = trackById(doc, trackId);
  if (!playsPatterns(t)) throw DocumentError("stutter", t.name + " plays no notes");
  const bool whole = to <= from;
  const bool pitched = !(t.instrument.kind == "builtin" &&
                         (t.instrument.name == "kick" || t.instrument.name == "hats" || t.instrument.name == "snare" || t.instrument.name == "drums"));
  std::vector<std::string> clips;
  for (const auto& c : t.clips)
    if (whole || (c.startTick < to && c.startTick + c.lengthTicks > from)) clips.push_back(c.id);
  int written = 0;
  std::set<std::string> done;
  for (const auto& id : clips) {
    const std::string pid = makeClipUnique(t, id);
    if (done.count(pid)) continue;
    done.insert(pid);
    const Clip* cp = nullptr;
    for (const auto& x : t.clips) if (x.id == id) cp = &x;
    if (!cp) continue;
    const Clip& c = *cp;
    Pattern& p = patternById(t, pid);
    std::set<std::string> ids;
    // Every place the note sounds in this clip: a looping clip plays the pattern again and again, and a clip that
    // starts mid-pattern plays its first notes in the next repeat (sweep: only the first pass counted).
    const int64_t len = std::max<int64_t>(1, p.lengthTicks);
    const int64_t origin = c.startTick - c.offsetTicks % len, end = c.startTick + c.lengthTicks;
    for (const auto& n : p.notes)
      for (int64_t base = origin; base + n.tick < end; base += len) {
        const int64_t at = base + n.tick;
        if (at < c.startTick) continue;
        if (whole || (at >= from && at < to)) { ids.insert(n.id); break; }
      }
    if (!ids.empty()) written += stutterNotes(p, ids, preset.recipe, preset.id, pitched);
  }
  return written;
}

// ---------------------------------------------------------------- VOICE STUTTER
namespace {
constexpr int64_t k2 = kPpq * 2, k4 = kPpq, k8 = kPpq / 2, k16 = kPpq / 4, k32 = kPpq / 8;
}

const std::vector<VoiceStutterPreset>& voiceStutterPresets() {
  // Windows, gaps, gates and gains from CORE-MANUAL plugins/plans/stutter-voice-stutter-recipes-and-research.md
  // section 4 (starting suggestions, not heard yet).
  static const std::vector<VoiceStutterPreset> presets = {
      {"word-repeat", "Spaced Word Repeat", "psytrance", "Two quiet fragments a quarter apart.",
       {0, 0}, {k4, k4}, 0.9, -6, -6, {}, false},
      {"prefix-return", "Prefix Return", "psytrance", "Two fragments on eighths, the second a little softer.",
       {0, 0}, {k8, k8}, 0.75, 0, -3, {}, false},
      {"syllable-repeat", "Syllable Repeat", "psytrance", "Three fragments on sixteenths, steady: the full-on "
       "ba-ba-ba.", {0, 0, 0}, {k16, k16, k16}, 0.65, 0, 0, {}, false},
      {"vocal-acceleration", "Vocal Acceleration", "psytrance", "Four fragments getting faster (1/8, 1/16, 1/32, 1/32) and "
       "louder.", {0, 0, 0, 0}, {k8, k16, k32, k32}, 0.65, -6, 0, {}, false},
      {"progressive-fragments", "Progressive Fragments", "psytrance", "A, A, then the longer B on eighths.",
       {0, 0, 1}, {k8, k8, k8}, 0.7, 0, 0, {}, false},
      {"fragment-dialogue", "Fragment Dialogue", "psytrance", "A, B, A, C on sixteenths with a breath after the second, "
       "alternating loud and soft.", {0, 1, 0, 2}, {k16, k8, k16, k16}, 0.6, 0, 0, {0, -3, 0, -3}, false},
      {"broken-voice", "Broken Voice", "darkpsycore", "Four fragments with uneven gaps and slightly shifted starts: a "
       "broken, glitching voice.", {0, 0, 0, 0}, {k16, k8, k16, k16}, 0.55, 0, 0, {}, true},
      {"slowing-word", "Slowing Word", "darkpsycore", "Four fragments slowing down (1/32 to 1/8) and fading.",
       {0, 0, 0, 0}, {k32, k16, k8, k8}, 0.65, 0, -6, {}, false},
      {"dense-fragments", "Dense Fragments", "darkpsycore", "Eight A/B fragments on 1/32 with one short rest in the "
       "middle, rising.", {0, 1, 0, 1, 0, 1, 0, 1}, {k32, k32, k32, k16, k32, k32, k32, k32}, 0.45, -6, 0, {}, false},
      {"fragment-return", "Fragment Return", "hitech", "A A B A C C on 1/32: fast hi-tech chopping.",
       {0, 0, 1, 0, 2, 2}, {k32, k32, k32, k32, k32, k32}, 0.5, 0, 0, {}, false},
      {"distant-word", "Distant Word", "slambient", "Two soft fragments a half note apart, far back.",
       {0, 0}, {k2, k2}, 0.8, -12, -12, {}, false},
      // With Core's pitch and freeze processors (the audio clip's pitchSemitones and freezeSeconds).
      {"vocal-climb", "Vocal Climb", "darkpsycore", "Six fragments speeding up (1/16 to 1/64) and climbing a fifth; the "
       "word comes back at its own pitch.", {0, 0, 0, 0, 0, 0}, {k16, k16, k32, k32, k32 / 2, k32 / 2}, 0.5, 0, 0, {}, false,
       {0, 1.4, 2.8, 4.2, 5.6, 7}, false},
      {"robotic-fragments", "Robotic Fragments", "hitech", "Four fragments on sixteenths at 0, +7, 0 and -5 semitones: a "
       "robotic melody.", {0, 0, 0, 0}, {k16, k16, k16, k16}, 0.6, 0, 0, {}, false, {0, 7, 0, -5}, false},
      {"vocal-freeze", "Vocal Freeze", "slambient", "The marked piece (a held vowel works best) frozen into a soft drone for "
       "four bars, then the word.", {0}, {16 * kPpq}, 0.95, -9, -9, {}, false, {}, true}};
  return presets;
}

const VoiceStutterPreset* findVoiceStutterPreset(const std::string& id) {
  for (const auto& p : voiceStutterPresets())
    if (id == p.id) return &p;
  return nullptr;
}

std::vector<std::string> voiceStutter(Document& doc, const std::string& trackId, const std::string& clipId, double fragmentStart,
                                      double fragmentSeconds, const VoiceStutterPreset& preset) {
  Track& t = trackById(doc, trackId);
  AudioClip* found = nullptr;
  for (auto& c : t.audioClips) if (c.id == clipId) found = &c;
  if (!found) throw DocumentError("audioClips", "no such audio clip");
  if (preset.fragments.empty() || preset.gaps.size() != preset.fragments.size()) throw DocumentError("preset", "is not a valid recipe");
  if (t.audioClips.size() + preset.fragments.size() > 1024) throw DocumentError("audioClips", "the track would pass 1024 audio clips");
  const AudioClip word = *found;
  const double clipFrom = word.offsetSeconds, clipTo = word.offsetSeconds + word.durationSeconds;
  if (!std::isfinite(fragmentStart) || !std::isfinite(fragmentSeconds)) throw DocumentError("fragment", "must be seconds");
  const double frag = std::clamp(fragmentSeconds, 0.01, std::max(0.01, word.durationSeconds));
  if (word.durationSeconds < 0.02) throw DocumentError("fragment", "the clip is too short to cut a fragment from");
  const double start = std::clamp(fragmentStart, clipFrom, std::max(clipFrom, clipTo - frag));
  uint32_t seed = 2166136261u;
  for (char ch : clipId) seed = (seed ^ (uint8_t)ch) * 16777619u;
  auto random = [&] { seed = seed * 1664525u + 1013904223u; return (double)seed / 4294967296.0; };  // 0..1
  std::vector<std::string> made;
  int64_t at = word.startTick;
  const size_t n = preset.fragments.size();
  for (size_t i = 0; i < n; ++i) {
    const int64_t gap = std::max<int64_t>(1, preset.gaps[i]);
    double from = start + preset.fragments[i] * frag;
    if (from + frag > clipTo + 1e-9) from = start;  // no room for B or C in this clip: A
    if (preset.scatter) from = std::clamp(from + (random() - 0.5) * 0.5 * frag, clipFrom, std::max(clipFrom, clipTo - frag));
    const double gapSeconds = secondsBetween(doc, (double)at, (double)(at + gap));
    const double dur = preset.freeze ? std::max(kMinClipSeconds, preset.gate * gapSeconds)  // a freeze outlasts its piece
                                     : std::clamp(std::min(frag, preset.gate * gapSeconds), kMinClipSeconds, std::max(kMinClipSeconds, clipTo - from));
    AudioClip e = word;
    e.id = "";
    e.startTick = at;
    e.offsetSeconds = from;
    e.durationSeconds = dur;
    const double gain = !preset.gainsDb.empty() ? preset.gainsDb[std::min(i, preset.gainsDb.size() - 1)]
                        : preset.gainFromDb + (preset.gainToDb - preset.gainFromDb) * (n > 1 ? (double)i / (double)(n - 1) : 1.0);
    e.gainDb = std::clamp(word.gainDb + gain, -60.0, 12.0);
    e.fadeInSeconds = std::min(preset.freeze ? 0.05 : 0.001, dur / 4);   // a cut never clicks; the attack stays
    e.fadeOutSeconds = std::min(preset.freeze ? 0.3 : 0.005, dur / 4);
    e.pitchSemitones = preset.pitches.empty() ? 0 : preset.pitches[std::min(i, preset.pitches.size() - 1)];
    e.freezeSeconds = preset.freeze ? frag : 0;
    e.extra["voiceStutter"] = corePluginState("voicestutter", {{"preset", preset.id}, {"role", "fragment"}, {"label", std::string(1, (char)('A' + preset.fragments[i]))}});
    const std::string id = duplicateAudioClip(doc, trackId, clipId, at);  // a fresh id, the same asset
    for (auto& c : trackById(doc, trackId).audioClips)
      if (c.id == id) { e.id = id; c = e; }
    made.push_back(id);
    at += gap;
  }
  for (auto& c : trackById(doc, trackId).audioClips)
    if (c.id == clipId) {
      c.startTick = at;
      c.extra["voiceStutter"] = corePluginState("voicestutter", {{"preset", preset.id}, {"role", "word"}});
      const int64_t end = audioClipEndTick(doc, c);
      if (end > doc.lengthTicks) {
        try { setSongLength(doc, (end + 4 * kPpq - 1) / (4 * kPpq) * (4 * kPpq)); } catch (const std::exception&) {}
      }
    }
  return made;
}

}  // namespace core::doc
