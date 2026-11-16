#include "Plugins.h"
#include <juce_events/juce_events.h>
#include <filesystem>

namespace core::app {

namespace {
constexpr int kScanTimeoutMs = 20000;     // per plugin file
constexpr int kQualifyTimeoutMs = 60000;  // per plugin file (a few seconds of audio per plugin, plus loading)

// Runs core-engine <mode> <file> with a time limit; false (and problem) if it hung or could not start.
bool runEngine(const juce::File& engine, const juce::String& mode, const juce::File& plugin, int timeoutMs, juce::String& output,
               int& exitCode, juce::String* problem, const juce::StringArray& extra = {}) {
  juce::ChildProcess child;
  juce::StringArray args{engine.getFullPathName(), mode, plugin.getFullPathName()};
  args.addArray(extra);
  if (!child.start(args, juce::ChildProcess::wantStdOut)) {
    if (problem) *problem = "the engine could not be started";
    return false;
  }
  std::thread reader([&] { output = child.readAllProcessOutput(); });
  const bool finished = child.waitForProcessToFinish(timeoutMs);
  if (!finished) child.kill();
  reader.join();
  if (!finished) { if (problem) *problem = "did not answer in " + juce::String(timeoutMs / 1000) + " s"; return false; }
  exitCode = (int)child.getExitCode();
  return true;
}

// The engine's answer is its last line that is JSON: a plugin may print its own text before it (Dexed
// prints a banner), even though the engine keeps stdout for the answer.
PluginCatalog::json lastJsonLine(const juce::String& output) {
  juce::StringArray lines;
  lines.addLines(output);
  for (int i = lines.size() - 1; i >= 0; --i) {
    const juce::String line = lines[i].trim();
    if (line.startsWithChar('[') || line.startsWithChar('{')) return PluginCatalog::json::parse(line.toStdString());
  }
  throw std::runtime_error("no JSON in the answer");
}

// The .vst3 files and bundles under a folder (a bundle is a folder named *.vst3: not looked into).
void findPlugins(const juce::File& folder, juce::Array<juce::File>& out, int depth = 0) {
  if (depth > 6) return;
  for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFilesAndDirectories)) {
    const juce::File f = entry.getFile();
    if (f.hasFileExtension(".vst3")) out.add(f);
    else if (f.isDirectory()) findPlugins(f, out, depth + 1);
  }
}
}  // namespace

PluginCatalog::PluginCatalog(juce::File engine) : engine_(std::move(engine)) {
  storeFile_ = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("plugins.json");
  folders_.push_back("C:\\Program Files\\Common Files\\VST3");  // where VST3 installers put them
  load();
}

PluginCatalog::~PluginCatalog() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
}

void PluginCatalog::load() {
  if (!storeFile_.existsAsFile()) return;
  try {
    const json j = json::parse(storeFile_.loadFileAsString().toStdString());
    std::lock_guard<std::mutex> g(mutex_);
    for (const auto& p : j.value("plugins", json::array())) if (p.is_object()) plugins_.push_back(p);
    for (const auto& f : j.value("folders", json::array()))
      if (f.is_string() && std::find(folders_.begin(), folders_.end(), juce::String::fromUTF8(f.get<std::string>().c_str())) == folders_.end())
        folders_.push_back(juce::String::fromUTF8(f.get<std::string>().c_str()));
    for (const auto& q : j.value("quarantine", json::array())) if (q.is_string()) quarantine_.push_back(juce::String::fromUTF8(q.get<std::string>().c_str()));
    const json problems = j.value("problems", json::object());
    for (auto it = problems.begin(); it != problems.end(); ++it)
      if (it.value().is_string()) problems_[juce::String::fromUTF8(it.key().c_str())] = juce::String::fromUTF8(it.value().get<std::string>().c_str());
  } catch (const std::exception&) {}  // a damaged list: found again by the next scan
}

void PluginCatalog::store() const {
  json j = {{"plugins", json::array()}, {"folders", json::array()}, {"quarantine", json::array()}, {"problems", json::object()}};
  {
    std::lock_guard<std::mutex> g(mutex_);
    for (const auto& p : plugins_) j["plugins"].push_back(p);
    for (const auto& f : folders_) j["folders"].push_back(f.toStdString());
    for (const auto& q : quarantine_) j["quarantine"].push_back(q.toStdString());
    for (const auto& [file, why] : problems_) j["problems"][file.toStdString()] = why.toStdString();
  }
  storeFile_.getParentDirectory().createDirectory();
  storeFile_.replaceWithText(juce::String::fromUTF8(j.dump(2).c_str()));
}

std::vector<PluginCatalog::json> PluginCatalog::instruments() const {
  std::lock_guard<std::mutex> g(mutex_);
  std::vector<json> out;
  for (const auto& p : plugins_) if (p.value("instrument", false)) out.push_back(p);
  return out;
}

std::vector<PluginCatalog::json> PluginCatalog::effects() const {
  std::lock_guard<std::mutex> g(mutex_);
  std::vector<json> out;
  for (const auto& p : plugins_) if (!p.value("instrument", false)) out.push_back(p);
  return out;
}

std::vector<juce::String> PluginCatalog::folders() const { std::lock_guard<std::mutex> g(mutex_); return folders_; }
std::vector<juce::String> PluginCatalog::quarantine() const { std::lock_guard<std::mutex> g(mutex_); return quarantine_; }
juce::String PluginCatalog::progress() const { std::lock_guard<std::mutex> g(mutex_); return progress_; }
juce::String PluginCatalog::problem(const juce::String& file) const {
  std::lock_guard<std::mutex> g(mutex_);
  auto it = problems_.find(file);
  return it == problems_.end() ? juce::String() : it->second;
}

void PluginCatalog::clearQuarantine() {
  { std::lock_guard<std::mutex> g(mutex_); quarantine_.clear(); problems_.clear(); }
  store();
}

void PluginCatalog::addFolder(const juce::File& folder) {
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (std::find(folders_.begin(), folders_.end(), folder.getFullPathName()) != folders_.end()) return;
    folders_.push_back(folder.getFullPathName());
  }
  store();
}

PluginCatalog::json PluginCatalog::scanFile(const juce::File& engine, const juce::File& plugin, juce::String* problem) {
  // The output is read on another thread (reading blocks until the child ends); this one keeps the time
  // and stops a plugin that hangs.
  juce::String output;
  int exitCode = 0;
  if (!runEngine(engine, "scan", plugin, kScanTimeoutMs, output, exitCode, problem)) return json();
  if (exitCode != 0) { if (problem) *problem = "the scan stopped (exit " + juce::String(exitCode) + ")"; return json(); }
  try {
    const json found = lastJsonLine(output);
    if (!found.is_array()) throw std::runtime_error("not a list");
    return found;
  } catch (const std::exception&) {
    if (problem) *problem = "the scan gave no answer";
    return json();
  }
}

PluginCatalog::json PluginCatalog::paramsOf(const juce::File& engine, const json& plugin, juce::String* problem) {
  juce::String output;
  int exitCode = 0;
  const juce::File file(juce::String::fromUTF8(plugin.value("file", std::string()).c_str()));
  const juce::String id = juce::String::fromUTF8(plugin.value("id", std::string()).c_str());
  if (!runEngine(engine, "params", file, kQualifyTimeoutMs, output, exitCode, problem, juce::StringArray{juce::String("--id"), id})) return json();
  if (exitCode != 0) { if (problem) *problem = "the plugin stopped while it was read (exit " + juce::String(exitCode) + ")"; return json(); }
  try {
    const json results = lastJsonLine(output);
    for (const auto& r : results)
      if (r.value("id", std::string()) == plugin.value("id", std::string())) {
        if (r.contains("params")) return r["params"];
        if (problem) *problem = juce::String::fromUTF8(r.value("problem", std::string("it did not load")).c_str());
        return json();
      }
    if (problem) *problem = "the plugin is not in its file any more (scan again)";
  } catch (const std::exception&) {
    if (problem) *problem = "the engine gave no answer";
  }
  return json();
}

juce::File PluginCatalog::rolesFile() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("roles.json");
}

core::doc::RoleMap PluginCatalog::roles(std::string* problem) {
  return core::doc::loadRoleMap(std::filesystem::path(rolesFile().getFullPathName().toWideCharPointer()), problem);
}

PluginCatalog::json PluginCatalog::qualifyFile(const juce::File& engine, const juce::File& plugin, juce::String* problem) {
  juce::String output;
  int exitCode = 0;
  if (!runEngine(engine, "qualify", plugin, kQualifyTimeoutMs, output, exitCode, problem)) return json();
  if (exitCode != 0 && exitCode != 3) {  // 3: answered, with a plugin found unsafe; anything else: it crashed
    if (problem) *problem = "crashed while it played (exit " + juce::String(exitCode) + ")";
    return json();
  }
  try {
    const json results = lastJsonLine(output);
    if (!results.is_array()) throw std::runtime_error("not a list");
    return results;
  } catch (const std::exception&) {
    if (problem) *problem = "the qualification gave no answer";
    return json();
  }
}

void PluginCatalog::scan(std::function<void(int, int)> onDone) {
  if (scanning_) return;
  if (worker_.joinable()) worker_.join();
  scanning_ = true;
  worker_ = std::thread([this, onDone] {
    juce::Array<juce::File> files;
    for (const auto& folder : folders()) if (juce::File(folder).isDirectory()) findPlugins(juce::File(folder), files);
    std::vector<json> found;
    std::vector<juce::String> failed;
    std::map<juce::String, juce::String> why;
    const auto setAside = quarantine();
    std::vector<json> known;  // what the last scan found: its qualification is kept while the version stays
    { std::lock_guard<std::mutex> g(mutex_); known = plugins_; why = problems_; }
    auto qualifiedBefore = [&](const json& p) -> json {
      for (const auto& k : known)
        if (k.value("id", "") == p.value("id", "") && k.value("version", "") == p.value("version", "") && k.contains("qualified"))
          return k["qualified"];
      return json();
    };
    for (int i = 0; i < files.size() && !stop_; ++i) {
      const juce::String path = files[i].getFullPathName();
      { std::lock_guard<std::mutex> g(mutex_); progress_ = juce::String(i + 1) + " / " + juce::String(files.size()) + ": " + files[i].getFileName(); }
      if (std::find(setAside.begin(), setAside.end(), path) != setAside.end()) { failed.push_back(path); continue; }
      juce::String problem;
      json list = scanFile(engine_, files[i], &problem);
      if (!list.is_array()) { failed.push_back(path); why[path] = problem; continue; }
      // Plugin protection: a plugin (or a version of it) not qualified yet plays on its own first.
      bool needed = false;
      for (auto& p : list) { const json q = qualifiedBefore(p); if (q.is_object()) p["qualified"] = q; else needed = true; }
      if (needed) {
        { std::lock_guard<std::mutex> g(mutex_); progress_ = juce::String(i + 1) + " / " + juce::String(files.size()) + ": testing " + files[i].getFileName(); }
        const json results = qualifyFile(engine_, files[i], &problem);
        if (!results.is_array()) { failed.push_back(path); why[path] = problem; continue; }
        for (auto& p : list)
          for (const auto& r : results)
            if (r.value("id", "") == p.value("id", "")) {
              p["qualified"] = {{"verdict", r.value("verdict", "unsafe")}, {"problems", r.value("problems", json::array())},
                                {"warnings", r.value("warnings", json::array())}, {"cpuPct", r.value("cpuPct", 0.0)}};
            }
      }
      bool unsafe = false;
      for (const auto& p : list) {
        const json q = p.value("qualified", json::object());
        if (q.value("verdict", "unsafe") != "ok") {
          unsafe = true;
          const json problems = q.value("problems", json::array());
          why[path] = juce::String::fromUTF8((p.value("name", std::string("plugin")) + ": " + (problems.empty() ? std::string("not qualified") : problems[0].get<std::string>())).c_str());
        }
      }
      if (unsafe) { failed.push_back(path); continue; }  // set aside until the quarantine is cleared (it is tried again then)
      why.erase(path);
      for (const auto& p : list) found.push_back(p);
    }
    if (stop_) { scanning_ = false; return; }  // the app is closing: the list from before stays
    {
      std::lock_guard<std::mutex> g(mutex_);
      plugins_ = found;
      quarantine_ = failed;
      problems_.clear();
      for (const auto& f : failed) if (why.count(f)) problems_[f] = why[f];
      progress_.clear();
    }
    store();
    const int foundCount = (int)found.size(), failedCount = (int)failed.size();
    scanning_ = false;
    juce::MessageManager::callAsync([onDone, foundCount, failedCount] { if (onDone) onDone(foundCount, failedCount); });
  });
}

} // namespace core::app
