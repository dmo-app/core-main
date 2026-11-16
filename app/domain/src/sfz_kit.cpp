#include "sfz_kit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace core::doc {

const std::vector<SfzKitNote>& sfzKitNotes() {
  static const std::vector<SfzKitNote> n = {{"kick", 36, "Kick"},   {"perc", 37, "Perc"}, {"snare", 38, "Snare"}, {"clap", 39, "Clap"},
                                            {"hat", 42, "Closed hat"}, {"tom", 45, "Tom"},  {"openhat", 46, "Open hat"}, {"crash", 49, "Crash"}};
  return n;
}

namespace {
bool hasStyle(const LibrarySample& s, const std::string& style) {
  return !style.empty() && std::find(s.info.styles.begin(), s.info.styles.end(), style) != s.info.styles.end();
}
std::string number(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}
}  // namespace

SfzKit makeSfzDrumKit(const SampleIndex& index, const SfzKitOptions& options) {
  const int perNote = std::clamp(options.perNote, 1, 16);
  SfzKit kit;
  std::string root = index.root;
  std::replace(root.begin(), root.end(), '\\', '/');
  if (!root.empty() && root.back() != '/') root += '/';
  std::string body;
  for (const auto& n : sfzKitNotes()) {
    std::vector<const LibrarySample*> pick;
    for (const auto& s : index.samples)
      if (s.info.role == n.role && !s.info.loop && s.seconds > 0 && s.seconds < 8) pick.push_back(&s);
    // The style's first, then by path (the same kit every time from the same library).
    std::stable_sort(pick.begin(), pick.end(), [&](const LibrarySample* a, const LibrarySample* b) {
      const bool sa = hasStyle(*a, options.style), sb = hasStyle(*b, options.style);
      return sa != sb ? sa : a->path < b->path;
    });
    if ((int)pick.size() > perNote) pick.resize((size_t)perNote);
    if (pick.empty()) continue;
    body += "\n// " + std::string(n.name) + "\n<group> key=" + std::to_string(n.note);
    if (pick.size() > 1) body += " seq_length=" + std::to_string(pick.size());
    if (std::string(n.role) == "hat") body += " group=1";             // a closed hat cuts the open one
    if (std::string(n.role) == "openhat") body += " group=2 off_by=1";
    body += "\n";
    int position = 1;
    for (const LibrarySample* s : pick) {
      body += "<region>";
      if (pick.size() > 1) body += " seq_position=" + std::to_string(position++);
      // The level the sampler gives a library sample: its peak at -3 dBFS, at most +18 dB.
      if (s->peakDb > kNotMeasured + 1)
        body += " volume=" + number(std::min(kSamplePeakDb - s->peakDb, kSampleMaxBoostDb));
      body += " sample=" + s->path + "\n";  // last on its line: a path may hold spaces
      kit.samples[n.note].push_back(s->path);
      ++kit.regions;
    }
  }
  if (kit.regions == 0) throw DocumentError("sfz", "the library has no one-shot kick, snare, clap, hat, tom, perc or crash");
  kit.text = "// Core AI Drums: made by Core from the sample library " + root +
             "\n// General MIDI notes: 36 kick, 37 perc, 38 snare, 39 clap, 42 closed hat, 45 tom, 46 open hat, 49 crash\n"
             "<control> default_path=" + root + "\n<global> loop_mode=one_shot\n" + body;
  return kit;
}

void writeSfzDrumKit(const SfzKit& kit, const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path temp = file.string() + ".part";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) throw DocumentError("sfz", "cannot write " + file.string());
    out << kit.text;
    if (!out) throw DocumentError("sfz", "cannot write " + file.string());
  }
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    throw DocumentError("sfz", "cannot write " + file.string());
  }
}

}  // namespace core::doc
