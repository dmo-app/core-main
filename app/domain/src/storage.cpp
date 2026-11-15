#include "storage.hpp"
#include <atomic>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace core::doc {

namespace fs = std::filesystem;

namespace {

// For messages: UTF-8, whatever characters the name has (path::string() throws on Windows for names
// outside the ANSI code page).
std::string show(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path withSuffix(const fs::path& p, const std::string& suffix) { return fs::path(p.native() + fs::path(suffix).native()); }

std::string readAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read " + show(path) + ".");
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

#ifdef _WIN32
std::string windowsError(DWORD code) {
  char* text = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                 MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), (LPSTR)&text, 0, nullptr);
  std::string message = text ? text : "error " + std::to_string(code);
  if (text) LocalFree(text);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) message.pop_back();
  return message + " (" + std::to_string(code) + ")";
}
#endif

// The bytes are on the disk, not only in a cache, before the file replaces the previous one.
void flushToDisk(const fs::path& path) {
#ifdef _WIN32
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw SaveError("Cannot reopen " + show(path) + ": " + windowsError(GetLastError()));
  const BOOL ok = FlushFileBuffers(h);
  const DWORD code = GetLastError();
  CloseHandle(h);
  if (!ok) throw SaveError("Cannot flush " + show(path) + ": " + windowsError(code));
#else
  (void)path;
#endif
}

void replaceWith(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
  if (fs::exists(target)) {
    // One step: the target gets the new content and the old one becomes the backup (an older
    // backup is replaced only when this succeeds).
    if (!ReplaceFileW(target.c_str(), temporary.c_str(), backupPath(target).c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
      throw SaveError("Cannot replace " + show(target) + ": " + windowsError(GetLastError()));
  } else if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
    throw SaveError("Cannot create " + show(target) + ": " + windowsError(GetLastError()));
  }
#else
  std::error_code e;
  if (fs::exists(target)) fs::copy_file(target, backupPath(target), fs::copy_options::overwrite_existing, e);
  fs::rename(temporary, target, e);
  if (e) throw SaveError("Cannot replace " + show(target) + ": " + e.message());
#endif
}

} // namespace

fs::path backupPath(const fs::path& path) { return withSuffix(path, ".bak"); }

OpenResult openFile(const fs::path& path) {
  const std::string text = readAll(path);
  const json value = json::parse(text, nullptr, false);
  if (value.is_discarded() || !value.is_object()) throw DocumentError("", "the file is not a Core document (not JSON)");
  OpenResult result;
  const std::string format = value.value("format", "");
  if (format == kDocumentFormat) {
    int migratedFrom = 0;
    result.document = parseDocument(value, &migratedFrom);
    if (migratedFrom) {  // never overwrite the old file: the migrated song is saved under a new name
      result.imported = true;
      result.notes.push_back("converted from schema " + std::to_string(migratedFrom) + ": each track's pattern is now pattern 1, placed over the song");
    }
  } else if (format == "core-project") {
    ImportReport report;
    result.document = importProjectV2(value, &report);
    result.imported = true;
    result.notes = report.converted;
    result.notes.insert(result.notes.end(), report.kept.begin(), report.kept.end());
  } else {
    throw DocumentError("format", "is not a Core document");
  }
  return result;
}

void writeFileSafely(const std::string& text, const fs::path& path, const std::function<bool(const std::string&)>& check) {
  static std::atomic<int> counter{0};
  // Same folder, so the final step is a rename on the same volume.
  const fs::path temporary = withSuffix(path, ".saving-" + std::to_string(counter++));
  auto discard = [&] { std::error_code ignored; fs::remove(temporary, ignored); };
  try {
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      if (!out) throw SaveError("Cannot write in " + show(path.parent_path()) + ".");
      out << text;
      out.close();
      if (!out) throw SaveError("Writing " + show(temporary) + " failed (disk full?).");
    }
    flushToDisk(temporary);
    // Read back what is on disk: only a complete, valid file may replace the previous one.
    const std::string back = readAll(temporary);
    if (back != text || !check(back)) throw SaveError("The saved copy does not match the document.");
    replaceWith(temporary, path);
  } catch (const SaveError&) {
    discard();
    throw;
  } catch (const std::exception& e) {
    discard();
    throw SaveError(e.what());
  }
}

void saveFile(const Document& document, const fs::path& path) {
  writeFileSafely(toJson(document).dump(2) + "\n", path,
                  [&](const std::string& text) { return sameContent(parseDocument(json::parse(text)), document); });
}

std::string readTextFile(const fs::path& path) { return readAll(path); }

} // namespace core::doc
