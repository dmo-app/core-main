#include "bridge.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace core::control {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr uintmax_t kMaxProposalBytes = 1u << 20;  // a proposal is a few calls: anything bigger is not one

bool safeId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; });
}

std::string nowIso() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A temporary file beside the target, then a rename over it.
bool writeAtomic(const fs::path& file, const std::string& text, std::string* error) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "cannot write " + u8(tmp);
      return false;
    }
    out << text;
    if (!out.flush()) {
      if (error) *error = "cannot write " + u8(tmp);
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (error) *error = "cannot replace " + u8(file);
    return false;
  }
  return true;
}

std::vector<fs::directory_entry> inboxFiles(const fs::path& folder) {
  std::vector<fs::directory_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(folder / "inbox", ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
    out.push_back(*it);
  }
  std::sort(out.begin(), out.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
    std::error_code e1, e2;
    const auto ta = a.last_write_time(e1), tb = b.last_write_time(e2);
    return ta != tb ? ta < tb : a.path() < b.path();
  });
  return out;
}

}  // namespace

bool writeState(const fs::path& folder, const json& state, std::string* error) {
  return writeAtomic(folder / "core-state.json", state.dump(1, ' ', false, json::error_handler_t::replace) + "\n", error);
}

std::vector<InboxItem> readInbox(const fs::path& folder) {
  std::vector<InboxItem> out;
  for (const auto& e : inboxFiles(folder)) {
    InboxItem item;
    item.file = e.path();
    item.id = u8(e.path().stem());
    std::error_code ec;
    if (e.file_size(ec) > kMaxProposalBytes) {
      item.error = "too big for a proposal";
    } else {
      std::ifstream in(e.path(), std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      const json j = json::parse(text.str(), nullptr, false);
      if (j.is_discarded() || !j.is_object()) item.error = "not a JSON object";
      else {
        item.proposal = j;
        if (j.contains("id") && j["id"].is_string() && safeId(j["id"].get<std::string>())) item.id = j["id"].get<std::string>();
      }
    }
    out.push_back(std::move(item));
  }
  return out;
}

bool finishProposal(const fs::path& folder, const InboxItem& item, const std::string& status, const json& result, std::string* error) {
  json out = result.is_object() ? result : json::object();
  out["id"] = item.id;
  out["status"] = status;
  out["at"] = nowIso();
  if (!item.proposal.is_null()) out["proposal"] = item.proposal;
  const std::string name = safeId(item.id) ? item.id : "proposal";
  if (!writeAtomic(folder / "outbox" / (name + ".json"), out.dump(1, ' ', false, json::error_handler_t::replace) + "\n", error)) return false;
  std::error_code ec;
  fs::remove(item.file, ec);
  return true;
}

std::string inboxSignature(const fs::path& folder) {
  std::ostringstream s;
  for (const auto& e : inboxFiles(folder)) {
    std::error_code ec;
    s << u8(e.path().filename()) << ":" << e.file_size(ec) << ":" << e.last_write_time(ec).time_since_epoch().count() << ";";
  }
  return s.str();
}

// ---------------------------------------------------------------- AICORE, Core's add-on

namespace {

json readJsonFile(const fs::path& file, uintmax_t maxBytes) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > maxBytes || ec) return nullptr;
  std::ifstream in(file, std::ios::binary);
  if (!in) return nullptr;
  std::stringstream text;
  text << in.rdbuf();
  json j = json::parse(text.str(), nullptr, false);
  return j.is_object() ? j : json(nullptr);
}

std::string fill(std::string s, const std::string& key, const std::string& value) {
  for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size())) s.replace(at, key.size(), value);
  return s;
}

std::vector<int> versionParts(const std::string& v) {
  std::vector<int> out;
  int n = 0;
  bool any = false;
  for (char c : v) {
    if (std::isdigit((unsigned char)c)) { n = n * 10 + (c - '0'); any = true; }
    else if (c == '.') { out.push_back(n); n = 0; any = false; }
    else break;
  }
  if (any || !out.empty()) out.push_back(n);
  return out;
}

}  // namespace

AicoreStatus readAicoreStatus(const fs::path& folder, std::chrono::system_clock::time_point now) {
  AicoreStatus st;
  const json j = readJsonFile(folder / "aicore-status.json", 64 * 1024);
  if (!j.is_object() || !j.contains("updatedAt") || !j["updatedAt"].is_number()) return st;
  const auto at = std::chrono::system_clock::time_point(std::chrono::milliseconds(j["updatedAt"].get<long long>()));
  const auto age = now - at;
  // Old: AICORE is not running. Far in the future: a clock that cannot be trusted.
  if (age > kAicoreStatusMaxAge || age < -std::chrono::seconds(60)) return st;
  st.running = true;
  // By type: "chatOpen": {} threw in the desktop's timer (audit 2026-11-07).
  st.chatOpen = j.contains("chatOpen") && j["chatOpen"].is_boolean() && j["chatOpen"].get<bool>();
  st.connected = j.contains("connected") && j["connected"].is_boolean() && j["connected"].get<bool>();
  if (j.contains("model") && j["model"].is_string()) st.model = j["model"].get<std::string>();
  return st;
}

bool writeAicoreCommand(const fs::path& folder, const std::string& page, std::string* error) {
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return writeAtomic(folder / "aicore-command.json", json{{"open", page}, {"at", ms}, {"from", "Core"}}.dump(2), error);
}

std::string samplesAddedSignature(const fs::path& folder) {
  std::error_code ec;
  const fs::path file = folder / "samples-added.json";
  const auto size = fs::file_size(file, ec);
  if (ec) return {};
  const auto time = fs::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}

std::optional<Addon> readAddon(const fs::path& folder, std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<Addon> { if (error) *error = why; return std::nullopt; };
  if (folder.empty()) return fail("no folder");
  const fs::path file = folder / "addon.json";
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) return fail("no addon.json in " + u8(folder));
  const json j = readJsonFile(file, 64 * 1024);
  if (!j.is_object()) return fail(u8(file) + " is not a JSON object");
  if (j.value("name", std::string()) != "AICORE") return fail(u8(file) + " is not AICORE's");
  const json open = j.value("open", json::object());
  if (!open.is_object() || !open.contains("program") || !open["program"].is_string() || open["program"].get<std::string>().empty())
    return fail(u8(file) + " says no program to open");
  Addon a;
  a.folder = folder;
  a.name = "AICORE";
  a.version = j.value("version", std::string());
  a.minCore = j.value("minCore", std::string());
  a.program = fill(open["program"].get<std::string>(), "{folder}", u8(folder));
  if (open.contains("args") && open["args"].is_array())
    for (const auto& x : open["args"]) if (x.is_string()) a.args.push_back(x.get<std::string>());
  return a;
}

std::optional<Addon> findAddon(const std::vector<fs::path>& folders) {
  for (const auto& f : folders)
    if (auto a = readAddon(f)) return a;
  return std::nullopt;
}

std::vector<std::string> addonArgs(const Addon& addon, const std::string& page) {
  std::vector<std::string> out;
  for (const auto& x : addon.args) out.push_back(fill(fill(x, "{folder}", u8(addon.folder)), "{page}", page));
  return out;
}

bool addonFits(const Addon& addon, const std::string& coreVersion) {
  const auto need = versionParts(addon.minCore), have = versionParts(coreVersion);
  if (need.empty()) return true;
  return !std::lexicographical_compare(have.begin(), have.end(), need.begin(), need.end());
}

}  // namespace core::control
