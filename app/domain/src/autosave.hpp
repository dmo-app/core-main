// Autosave (M6): while a song has unsaved changes, a copy is written now and then to a folder of the
// user's (never next to the song, never over it). A clean end removes it; one left behind after a crash
// is offered back on the next start. The song file itself is only ever written by Save.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

constexpr const char* kAutosaveExtension = ".core-autosave";

struct AutosaveInfo {
  std::filesystem::path file;  // the autosave itself
  std::string session;         // the app session that wrote it
  int64_t process = 0;         // its process id (still running: not a crash)
  std::string songPath;        // UTF-8; the song's file, "" if it was never saved
  std::string name;            // the song's name, for the question
  int64_t savedAtMs = 0;       // when (ms since 1970)
};

// One file per session ("<session>.core-autosave"), written safely (temporary file, read back, replace).
// Throws SaveError.
void writeAutosave(const std::filesystem::path& folder, const std::string& session, int64_t process, const Document& document,
                   const std::string& songPath, int64_t nowMs);
// Every autosave in the folder except this session's, newest first. Unreadable files are left out.
std::vector<AutosaveInfo> findAutosaves(const std::filesystem::path& folder, const std::string& exceptSession = "");
// The document kept in an autosave. Throws DocumentError, FutureVersionError or std::runtime_error.
Document readAutosave(const std::filesystem::path& file);
// Removes the autosave (and its .bak); nothing happens if there is none.
void removeAutosave(const std::filesystem::path& file);
std::filesystem::path autosavePath(const std::filesystem::path& folder, const std::string& session);

// When the next autosave is due. A copy counts as written only after its write succeeded: a failed one
// (full disk, locked folder) stays due and is tried again at the next check, even if the song did not
// change. Used on one thread (the window's); the write itself may run elsewhere and report back.
class AutosaveTracker {
public:
  // True when `text` (the song as saved) should be written now: not already on disk, and no write running.
  bool due(const std::string& text) const { return !writing_ && text != written_; }
  void started(std::string text) { writing_ = true; pending_ = std::move(text); attempted_ = true; }
  // The write's result. Returns true when the user should hear about it: the first failure in a row,
  // or the first success after failures.
  bool finished(bool ok, const std::string& error = "");
  // The autosave was removed (the song was saved or closed): nothing of this session is on disk.
  void removed() { written_.clear(); pending_.clear(); attempted_ = false; failures_ = 0; lastError_.clear(); }

  bool writing() const { return writing_; }
  bool mayHaveFile() const { return attempted_; }  // something of this session may be in the folder
  int failures() const { return failures_; }        // failed writes in a row (0: the last one worked)
  const std::string& lastError() const { return lastError_; }

private:
  std::string written_, pending_, lastError_;
  bool writing_ = false, attempted_ = false;
  int failures_ = 0;
};

} // namespace core::doc
