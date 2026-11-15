// core-clips: makes training clips (20-30 s excerpts with their facts) from the style templates, for AICORE to turn
// into a training set for ACE (LoRA). Diogo, 2026-11-05. Each clip is a <id>.wav rendered by core-engine and a
// <id>.json with everything Core knows about it (format core-training-clip v1, docs/contracts/core-training-clip.md);
// manifest.jsonl lists them. Clips that fail the checks (silence, clipping) are not
// kept: they are listed in the manifest with why. Runs below normal priority (Diogo may be listening).
//
//   core-clips --out <folder> [--templates <folder>]... [--only id,id] [--style "Hi-tech"] [--kinds kbbb,groove,full] [--count N]
//              [--seed N] [--min 20] [--max 30] [--index samples-index.json] [--library folder] [--swap-samples] [--axes a,b] [--no-randomize] [--batch name]
//              [--engine core-engine] [--dry-run]
//
// --count is per template and kind; seeds go seed, seed+1... The batch goes to <out>/<batch> (default: the date
// and time). --dry-run writes the facts and the projects without rendering.
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include "document.hpp"
#include "storage.hpp"
#include "templates.hpp"
#include "training_clips.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace core::doc;
namespace fs = std::filesystem;

static fs::path pathOf(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
static std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
static std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream in(s);
  for (std::string part; std::getline(in, part, ',');) if (!part.empty()) out.push_back(part);
  return out;
}
static std::string quoted(const fs::path& p) { return "\"" + utf8(p) + "\""; }

// Runs a command line (UTF-8), its output to files. Returns its exit code.
static int run(const std::string& line) {
#ifdef _WIN32
  const std::wstring w = pathOf("\"" + line + "\"").wstring();  // cmd /c drops the outer quotes
  return _wsystem(w.c_str());
#else
  return std::system(line.c_str());
#endif
}

static std::string readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main(int argc, char** argv) {
  std::vector<std::string> templateDirs, only, kinds = {"kbbb", "groove"}, axes, styles;
  std::string out, indexFile, libraryDir, batch, engine;
  int count = 5;
  uint64_t seed = 1;
  double minS = 20, maxS = 30;
  bool randomize = true, dry = false, swap = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
      return argv[++i];
    };
    try {
      if (a == "--out") out = next();
      else if (a == "--templates") templateDirs.push_back(next());
      else if (a == "--only") only = split(next());
      else if (a == "--kinds") kinds = split(next());
      else if (a == "--count") count = std::stoi(next());
      else if (a == "--seed") seed = std::stoull(next());
      else if (a == "--min") minS = std::stod(next());
      else if (a == "--max") maxS = std::stod(next());
      else if (a == "--index") indexFile = next();
      else if (a == "--library") libraryDir = next();
      else if (a == "--swap-samples") swap = true;
      else if (a == "--batch") batch = next();
      else if (a == "--engine") engine = next();
      else if (a == "--axes") axes = split(next());
      else if (a == "--style") styles = split(next());
      else if (a == "--no-randomize") randomize = false;
      else if (a == "--dry-run") dry = true;
      else throw std::runtime_error("unknown option " + a);
    } catch (const std::exception& e) {
      std::cerr << "core-clips: " << e.what() << "\n";
      return 2;
    }
  }
  if (out.empty() || count < 1 || count > 1000) {
    std::cerr << "usage: core-clips --out <folder> [--templates <folder>]... [--only id,id] [--style name] [--kinds kbbb,groove,full] [--count N] "
                 "[--seed N] [--min 20] [--max 30] [--index samples-index.json] [--library folder] [--swap-samples] [--axes a,b] [--no-randomize] [--batch name] [--engine core-engine] [--dry-run]\n";
    return 2;
  }
#ifdef _WIN32
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);  // the engine it starts inherits it
#endif
  try {
    const fs::path self = fs::absolute(pathOf(argv[0])).parent_path();
    if (templateDirs.empty()) templateDirs.push_back(utf8(self / "templates"));
    if (engine.empty()) {
#ifdef _WIN32
      engine = utf8(self / "core-engine.exe");
#else
      engine = utf8(self / "core-engine");
#endif
    }
    // Templates: the first folder's, then the others' (a later one with the same id takes its place).
    std::map<std::string, SongTemplate> templates;
    for (const auto& dir : templateDirs) {
      const TemplateLibrary lib = loadTemplates(pathOf(dir), {});
      for (const auto& p : lib.problems) std::cerr << "skipped " << utf8(p.file) << ": " << p.message << "\n";
      for (const auto& e : lib.templates) templates[e.tmpl.info.id] = e.tmpl;
    }
    if (!only.empty()) {
      const std::set<std::string> keep(only.begin(), only.end());
      for (auto it = templates.begin(); it != templates.end();) it = keep.count(it->first) ? std::next(it) : templates.erase(it);
    }
    if (!styles.empty()) {  // "Hi-tech", "hi-tech": the template's style, any case
      auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
      std::set<std::string> keep;
      for (const auto& s : styles) keep.insert(lower(s));
      for (auto it = templates.begin(); it != templates.end();) it = keep.count(lower(it->second.info.style)) ? std::next(it) : templates.erase(it);
    }
    if (templates.empty()) throw std::runtime_error("no templates");

    SampleIndex index;
    const SampleIndex* library = nullptr;
    if (!indexFile.empty()) {
      index = indexFromJson(nlohmann::json::parse(readTextFile(pathOf(indexFile))));
      resolveCatalog(index);
      library = &index;
    }
    // --library: the folder is read again first (files moved since the index was written are found by content and
    // keep their catalogue and measurements). The result is only used here; the app's index file is not written.
    if (!libraryDir.empty()) {
      std::cout << "reading the sample library " << libraryDir << " ...\n" << std::flush;
      const SampleIndex previous = index;
      index = scanLibrary(pathOf(libraryDir), library ? &previous : nullptr);
      resolveCatalog(index);
      library = &index;
      std::cout << index.samples.size() << " samples\n" << std::flush;
    }

    if (batch.empty()) {
      const std::time_t now = std::time(nullptr);
      std::tm tm{};
#ifdef _WIN32
      localtime_s(&tm, &now);
#else
      localtime_r(&now, &tm);
#endif
      std::ostringstream b;
      b << std::put_time(&tm, "%Y-%m-%d %H%M%S");
      batch = b.str();
    }
    const fs::path dir = pathOf(out) / pathOf(batch);
    const fs::path work = dir / ".work";
    fs::create_directories(work);
    std::ofstream manifest(dir / "manifest.jsonl", std::ios::app | std::ios::binary);

    int made = 0, rejected = 0, failed = 0;
    for (const auto& [id, tmpl] : templates)
      for (const auto& kind : kinds)
        for (int n = 0; n < count; ++n) {
          TrainingClipRequest rq;
          rq.kind = kind;
          rq.seed = seed + (uint64_t)n;
          rq.minSeconds = minS;
          rq.maxSeconds = maxS;
          rq.randomize = randomize;
          rq.axes = axes;
          rq.library = library;
          rq.swapSamples = swap;
          TrainingClip clip;
          try {
            clip = buildTrainingClip(tmpl, rq);
          } catch (const std::exception& e) {
            std::cerr << id << " " << kind << " seed " << rq.seed << ": " << e.what() << "\n";
            ++failed;
            break;  // the same problem for every seed of this template and kind
          }
          const fs::path project = work / pathOf(clip.id + ".project.json");
          const fs::path wav = dir / pathOf(clip.id + ".wav");
          const fs::path facts = dir / pathOf(clip.id + ".json");
          {
            std::ofstream p(project, std::ios::binary);
            p << toProjection(clip.doc).dump() << "\n";
          }
          std::string why;
          if (!dry) {
            const fs::path log = work / pathOf(clip.id + ".render.json"), err = work / pathOf(clip.id + ".render.txt");
            const std::string line = quoted(pathOf(engine)) + " render " + quoted(project) + " " + quoted(wav) + " --bits 24 --start-tick " +
                                     std::to_string(clip.startTick) + " --end-tick " + std::to_string(clip.endTick) + " --tail-ms 0 > " +
                                     quoted(log) + " 2> " + quoted(err);
            const int code = run(line);
            json result;
            try { result = json::parse(readAll(log)); } catch (...) {}
            if (code != 0 || !fs::exists(wav)) {
              why = "render failed: " + readAll(err) + (result.is_object() ? result.value("error", std::string()) : std::string());
            } else {
              json missing = json::array();
              if (result.contains("result") && result["result"].is_object()) {
                for (const auto& m : result["result"].value("missingAssets", json::array())) missing.push_back(m);
              }
              try {
                fadeWavEdges(wav, 10);
                why = checkTrainingClip(wav, clip.facts, missing);
              } catch (const std::exception& e) {
                why = e.what();
              }
            }
          }
          json line = {{"id", clip.id}, {"kind", kind}, {"style", clip.facts.value("style", "")}, {"template", id}, {"seed", rq.seed},
                       {"bpm", clip.facts.value("bpm", 0.0)}, {"bars", clip.bars}, {"seconds", clip.facts.value("seconds", 0.0)}};
          if (why.empty()) {
            std::ofstream f(facts, std::ios::binary);
            f << clip.facts.dump(2) << "\n";
            line["wav"] = dry ? "" : clip.id + ".wav";
            line["json"] = clip.id + ".json";
            line["description"] = clip.facts.value("description", "");
            std::error_code ec;  // the project and the engine's log are only kept for a clip that failed
            for (const char* ext : {".project.json", ".render.json", ".render.txt"}) fs::remove(work / pathOf(clip.id + ext), ec);
            ++made;
            std::cout << "ok  " << clip.id << "  " << clip.bars << " bars, " << std::fixed << std::setprecision(1) << clip.seconds << " s\n";
          } else {
            std::error_code ec;
            fs::remove(wav, ec);
            line["rejected"] = why;
            ++rejected;
            std::cout << "no  " << clip.id << "  " << why << "\n";
          }
          manifest << line.dump() << "\n";
          manifest.flush();
        }
    {
      std::error_code ec;
      if (fs::is_empty(work, ec)) fs::remove(work, ec);
    }
    std::cout << made << " clips in " << utf8(dir) << (rejected ? ", " + std::to_string(rejected) + " rejected" : "")
              << (failed ? ", " + std::to_string(failed) + " template/kind pairs could not be made" : "") << "\n";
    return made > 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "core-clips: " << e.what() << "\n";
    return 1;
  }
}
