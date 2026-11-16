#include "templates.hpp"
#include <algorithm>
#include <cctype>
#include <system_error>
#include "edit.hpp"
#include "template_sheet.hpp"
#include "storage.hpp"

namespace core::doc {

namespace fs = std::filesystem;

namespace {

bool validId(const std::string& id) {
  if (id.empty() || id.size() > 40) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

std::string text(json& value, const char* key, size_t maxLength, bool required) {
  json v = value.contains(key) ? value[key] : json();
  value.erase(key);
  if (v.is_null()) {
    if (required) throw DocumentError(key, "is required");
    return "";
  }
  if (!v.is_string()) throw DocumentError(key, "must be text");
  std::string s = v.get<std::string>();
  if (s.size() > maxLength) throw DocumentError(key, "is longer than " + std::to_string(maxLength) + " bytes (an accented letter counts as 2)");
  return s;
}

} // namespace

SongTemplate parseTemplate(const json& value) {
  if (!value.is_object()) throw DocumentError("", "a template must be a JSON object");
  json rest = value;
  if (text(rest, "format", 64, true) != kTemplateFormat) throw DocumentError("format", std::string("is not ") + kTemplateFormat);
  const json version = rest.value("schemaVersion", json());
  rest.erase("schemaVersion");
  if (!version.is_number_integer()) throw DocumentError("schemaVersion", "must be a whole number");
  // Read as 64 bits: 2^32 + 1 is not 1 (sweep).
  const int64_t schema = version.get<int64_t>();
  if (schema > kTemplateSchemaVersion) throw FutureVersionError((int)std::min<int64_t>(schema, 1 << 30));
  if (schema < 1) throw DocumentError("schemaVersion", "must be 1");

  SongTemplate t;
  t.info.id = text(rest, "id", 40, true);
  if (!validId(t.info.id)) throw DocumentError("id", "must be 1-40 letters, digits, '_' or '-'");
  const json v = rest.value("version", json(1));
  rest.erase("version");
  if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 1000000) throw DocumentError("version", "must be a whole number from 1");
  t.info.version = v.get<int>();
  t.info.name = text(rest, "name", 80, true);
  if (t.info.name.empty()) throw DocumentError("name", "cannot be empty");
  t.info.style = text(rest, "style", 40, false);
  t.info.description = text(rest, "description", 2000, false);
  if (rest.contains("source")) {
    if (!rest["source"].is_object()) throw DocumentError("source", "must be an object");
    t.info.source = rest["source"];
    rest.erase("source");
  }
  if (!rest.contains("document")) throw DocumentError("document", "is required");
  try {
    int migratedFrom = 0;
    t.document = parseDocument(rest["document"], &migratedFrom);
    if (migratedFrom) throw DocumentError("schemaVersion", "a template holds a schema 2 or 3 document");
  } catch (const FutureVersionError&) {
    throw;
  } catch (const DocumentError& e) {
    throw DocumentError(e.path.empty() ? "document" : "document." + e.path,
                        std::string(e.what()).substr(e.path.empty() ? 0 : e.path.size() + 2));
  }
  if (!t.document.assets.empty()) throw DocumentError("document.assets", "a template carries no audio files");
  rest.erase("document");
  t.info.extra = rest;
  return t;
}

json toJson(const SongTemplate& t) {
  json out = t.info.extra;
  out["format"] = kTemplateFormat;
  out["schemaVersion"] = kTemplateSchemaVersion;
  out["id"] = t.info.id;
  out["version"] = t.info.version;
  out["name"] = t.info.name;
  if (!t.info.style.empty()) out["style"] = t.info.style;
  if (!t.info.description.empty()) out["description"] = t.info.description;
  if (!t.info.source.empty()) out["source"] = t.info.source;
  out["document"] = toJson(t.document);
  return out;
}

Document documentFromTemplate(const SongTemplate& t) {
  Document d = t.document;
  d.name = "";
  d.loop.enabled = false;
  d.ui = json::object();
  d.extra["template"] = {{"id", t.info.id}, {"version", t.info.version}};
  if (!t.info.style.empty()) d.extra["template"]["style"] = t.info.style;  // what the sample suggestions look for
  return d;
}

SongTemplate templateFromDocument(const Document& song, TemplateInfo info) {
  if (!validId(info.id)) throw DocumentError("id", "must be 1-40 letters, digits, '_' or '-'");
  if (info.name.empty()) throw DocumentError("name", "cannot be empty");
  SongTemplate t;
  t.info = std::move(info);
  t.document = song;
  t.document.tracks.erase(std::remove_if(t.document.tracks.begin(), t.document.tracks.end(),
                                         [](const Track& tr) { return tr.instrument.kind == "audio"; }),
                          t.document.tracks.end());
  if (t.document.tracks.empty()) throw DocumentError("tracks", "a template needs at least one track that is not audio");
  t.document.assets.clear();
  t.document.name = t.info.name;
  t.document.loop.enabled = false;
  t.document.ui = json::object();
  t.document.extra.erase("template");  // the song's own origin; the template has its id
  return t;
}

SongTemplate templateFromRange(const Document& song, TemplateInfo info, int64_t startTick, int64_t endTick) {
  if (startTick < 0 || endTick > song.lengthTicks || endTick - startTick < kPpq)
    throw DocumentError("range", "the passage must be inside the song and at least one beat long");
  Document part = song;
  if (endTick < part.lengthTicks) deleteTime(part, endTick, part.lengthTicks - endTick);
  if (startTick > 0) deleteTime(part, 0, startTick);
  part.loop.startTick = 0;  // a loop outside the passage collapses in the cut: the template's loop is the passage (off)
  part.loop.endTick = part.lengthTicks;
  if (!info.source.is_object()) info.source = json::object();
  info.source["from"] = song.name;
  info.source["startTick"] = startTick;
  info.source["endTick"] = endTick;
  return templateFromDocument(part, std::move(info));
}

std::string templateIdFromName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if (std::isalnum(c) && c < 128) out += (char)std::tolower(c);
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.size() > 40) out.resize(40);
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "template" : out;
}

const TemplateEntry* TemplateLibrary::find(const std::string& id) const {
  for (const auto& e : templates) if (e.tmpl.info.id == id) return &e;
  return nullptr;
}

namespace {

void readFolder(const fs::path& folder, bool builtin, std::vector<TemplateEntry>& out, std::vector<TemplateProblem>& problems,
                bool withBases = true) {
  std::error_code e;
  if (folder.empty() || !fs::is_directory(folder, e)) return;
  std::vector<fs::path> files;
  for (fs::directory_iterator it(folder, e), end; !e && it != end; it.increment(e)) {
    const std::string name = it->path().filename().string();
    const std::string ext = kTemplateExtension;
    if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    try {
      const json value = json::parse(readTextFile(f), nullptr, false);
      if (value.is_discarded()) throw DocumentError("", "not JSON");
      TemplateEntry entry{parseTemplate(value), f, builtin, false};
      if (builtin && !withBases && isBaseTemplate(entry.tmpl)) continue;
      if (std::any_of(out.begin(), out.end(), [&](const TemplateEntry& x) { return x.builtin == builtin && x.tmpl.info.id == entry.tmpl.info.id; }))
        throw DocumentError("id", "'" + entry.tmpl.info.id + "' is used by another template in the same folder");
      out.push_back(std::move(entry));
    } catch (const std::exception& ex) {
      problems.push_back({f, ex.what()});
    }
  }
}

} // namespace

namespace {

fs::path hiddenFile(const fs::path& userFolder) { return userFolder / "hidden.json"; }

// The ids of the built-in templates the user deleted. A file that cannot be read hides nothing.
std::vector<std::string> readHidden(const fs::path& userFolder) {
  std::vector<std::string> ids;
  std::error_code e;
  if (userFolder.empty() || !fs::exists(hiddenFile(userFolder), e)) return ids;
  try {
    const json value = json::parse(readTextFile(hiddenFile(userFolder)), nullptr, false);
    if (value.is_object() && value.contains("hidden") && value["hidden"].is_array())
      for (const auto& id : value["hidden"]) if (id.is_string()) ids.push_back(id.get<std::string>());
  } catch (const std::exception&) {
  }
  return ids;
}

void writeHidden(const fs::path& userFolder, std::vector<std::string> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::error_code e;
  fs::create_directories(userFolder, e);
  if (e) throw SaveError("Cannot create the templates folder: " + e.message());
  const std::string text = json{{"hidden", ids}}.dump(2) + "\n";
  writeFileSafely(text, hiddenFile(userFolder), [&](const std::string& written) { return written == text; });
}

} // namespace

bool isBaseTemplate(const SongTemplate& t) {
  return t.info.source.is_object() && t.info.source.value("base", false);
}

TemplateLibrary loadTemplates(const fs::path& builtinFolder, const fs::path& userFolder, bool withBases) {
  TemplateLibrary lib;
  std::vector<TemplateEntry> builtins, users;
  readFolder(builtinFolder, true, builtins, lib.problems, withBases);
  const std::vector<std::string> hidden = readHidden(userFolder);
  builtins.erase(std::remove_if(builtins.begin(), builtins.end(), [&](const TemplateEntry& b) {
                   return std::find(hidden.begin(), hidden.end(), b.tmpl.info.id) != hidden.end();
                 }), builtins.end());
  readFolder(userFolder, false, users, lib.problems);
  auto byName = [](const TemplateEntry& a, const TemplateEntry& b) { return a.tmpl.info.name < b.tmpl.info.name; };
  std::stable_sort(builtins.begin(), builtins.end(), byName);
  std::stable_sort(users.begin(), users.end(), byName);
  for (auto& b : builtins) {
    auto u = std::find_if(users.begin(), users.end(), [&](const TemplateEntry& x) { return x.tmpl.info.id == b.tmpl.info.id; });
    if (u == users.end()) {
      lib.templates.push_back(std::move(b));
    } else {  // the user's edited copy, in the built-in one's place
      u->overridesBuiltin = true;
      lib.templates.push_back(std::move(*u));
      users.erase(u);
    }
  }
  for (auto& u : users) lib.templates.push_back(std::move(u));
  return lib;
}

fs::path saveTemplate(SongTemplate t, const TemplateLibrary& library, const fs::path& userFolder) {
  if (!validId(t.info.id)) throw SaveError("The template id '" + t.info.id + "' is not valid.");
  if (const TemplateEntry* existing = library.find(t.info.id)) t.info.version = existing->tmpl.info.version + 1;
  std::error_code e;
  fs::create_directories(userFolder, e);
  if (e) throw SaveError("Cannot create the templates folder: " + e.message());
  const fs::path file = userFolder / (t.info.id + kTemplateExtension);
  t.info.extra["ficha"] = templateSheet(t);  // every template carries its sheet (Diogo 2026-11-09), made at this version
  const json value = toJson(t);
  writeFileSafely(value.dump(2) + "\n", file, [&](const std::string& written) {
    const SongTemplate back = parseTemplate(json::parse(written));
    return back.info.version == t.info.version && sameContent(back.document, t.document);
  });
  return file;
}

void deleteTemplate(const TemplateLibrary& library, const std::string& id, const fs::path& userFolder) {
  const TemplateEntry* entry = library.find(id);
  if (!entry) throw SaveError("There is no template '" + id + "'.");
  if (!entry->builtin) {
    std::error_code e;
    const fs::path bak = backupPath(entry->file);
    fs::remove(bak, e);
    fs::rename(entry->file, bak, e);
    if (e) throw SaveError("Cannot move " + entry->file.string() + " aside: " + e.message());
  }
  if (entry->builtin || entry->overridesBuiltin) {  // the built-in one under it must not come back
    std::vector<std::string> hidden = readHidden(userFolder);
    hidden.push_back(id);
    writeHidden(userFolder, hidden);
  }
}

int restoreBuiltinTemplates(const fs::path& userFolder) {
  const std::vector<std::string> hidden = readHidden(userFolder);
  if (hidden.empty()) return 0;
  writeHidden(userFolder, {});
  return (int)hidden.size();
}

} // namespace core::doc
