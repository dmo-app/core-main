#include "autosave.hpp"
#include <algorithm>
#include "storage.hpp"

namespace core::doc {

namespace fs = std::filesystem;

namespace {
// {format, version, session, process, songPath, name, savedAt, document}
constexpr const char* kFormat = "core-autosave";
}  // namespace

fs::path autosavePath(const fs::path& folder, const std::string& session) { return folder / fs::u8path(session + kAutosaveExtension); }

void writeAutosave(const fs::path& folder, const std::string& session, int64_t process, const Document& document,
                   const std::string& songPath, int64_t nowMs) {
  std::error_code e;
  fs::create_directories(folder, e);
  if (e) throw SaveError("Cannot create the autosave folder: " + e.message());
  json doc = toJson(document);
  // Only what the reader takes back is kept: an autosave that recovery then refuses is no autosave (audit 2026-11-07).
  try { (void)parseDocument(doc); } catch (const std::exception& e) { throw SaveError(std::string("The song cannot be read back: ") + e.what()); }
  const json saved = {{"format", kFormat}, {"version", 1}, {"session", session}, {"process", process}, {"songPath", songPath},
                      {"name", document.name}, {"savedAt", nowMs}, {"document", std::move(doc)}};
  const std::string text = saved.dump();
  writeFileSafely(text, autosavePath(folder, session), [&](const std::string& written) { return written == text; });
}

std::vector<AutosaveInfo> findAutosaves(const fs::path& folder, const std::string& exceptSession) {
  std::vector<AutosaveInfo> out;
  std::error_code e;
  if (folder.empty() || !fs::is_directory(folder, e)) return out;
  for (const auto& entry : fs::directory_iterator(folder, e)) {
    if (!entry.is_regular_file(e) || entry.path().extension() != kAutosaveExtension) continue;
    try {
      const json j = json::parse(readTextFile(entry.path()));
      if (j.value("format", std::string()) != kFormat || !j.contains("document")) continue;
      AutosaveInfo info;
      info.file = entry.path();
      info.session = j.value("session", std::string());
      if (info.session == exceptSession) continue;
      info.process = j.value("process", (int64_t)0);
      info.songPath = j.value("songPath", std::string());
      info.name = j.value("name", std::string());
      info.savedAtMs = j.value("savedAt", (int64_t)0);
      out.push_back(std::move(info));
    } catch (const std::exception&) {  // half written, or not ours: left alone
    }
  }
  std::sort(out.begin(), out.end(), [](const AutosaveInfo& a, const AutosaveInfo& b) { return a.savedAtMs > b.savedAtMs; });
  return out;
}

Document readAutosave(const fs::path& file) {
  // Audit 2 (2026-11-16): a damaged autosave says so in plain words (the reader shows it and leaves the file).
  const json j = json::parse(readTextFile(file), nullptr, false);
  if (j.is_discarded()) throw std::runtime_error("The autosave is damaged (not readable JSON).");
  if (!j.is_object() || !j.contains("format") || j["format"] != kFormat || !j.contains("document"))
    throw std::runtime_error("This is not an autosave of Core.");
  return parseDocument(j["document"]);
}

void removeAutosave(const fs::path& file) {
  std::error_code e;
  fs::remove(file, e);
  fs::remove(backupPath(file), e);
}

bool AutosaveTracker::finished(bool ok, const std::string& error) {
  writing_ = false;
  if (ok) {
    written_ = std::move(pending_);
    pending_.clear();
    const bool recovered = failures_ > 0;
    failures_ = 0;
    lastError_.clear();
    return recovered;
  }
  pending_.clear();  // not on disk: the same text is due again
  lastError_ = error.empty() ? std::string("unknown error") : error;
  return ++failures_ == 1;
}

} // namespace core::doc
