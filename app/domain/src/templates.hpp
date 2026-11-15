// Song templates ("core-template", schema 1): a starting point for a new song in a style (darkpsycore,
// slambient, hi-tech...). A template is a whole core-document (tempo, sections, tracks with their
// instruments and parameters, patterns, clips, effect chains, mix and automation) plus a name, a style and
// a version. Everything in it is ordinary document content, so a song made from it is edited like any other.
// Built-in templates ship as files in the app's "templates" folder; the user's own (or an edited copy of a
// built-in one, same id) live in the user's templates folder and take its place in the list.
// Contract: docs/contracts/core-template.md.
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

constexpr const char* kTemplateFormat = "core-template";
constexpr int kTemplateSchemaVersion = 1;
constexpr const char* kTemplateExtension = ".core-template.json";

struct TemplateInfo {
  std::string id;            // [A-Za-z0-9_-]{1,40}: the file name, and what a song made from it remembers
  int version = 1;           // goes up by one each time the template is saved again
  std::string name;          // shown in the Library's Templates tab
  std::string style;         // free text: "darkpsycore", "slambient", "hitech"...
  std::string description;
  // Where it came from: {"kind": "builtin" | "user" | "analysis", ...}. "analysis" is for templates made
  // from the stems of a reference track (not implemented yet): they are built with the public document
  // edits and then templateFromDocument(), like any other.
  json source = json::object();
  json extra = json::object();
};

struct SongTemplate {
  TemplateInfo info;
  Document document;
};

// Validates (the document with the same rules as a song); throws DocumentError (paths start with
// "document." inside the song) or FutureVersionError.
SongTemplate parseTemplate(const json& value);
json toJson(const SongTemplate& t);  // sorted keys: the same template always gives the same text

// A new, unsaved song from the template. It remembers {"id", "version"} of the template in its
// "template" field; the loop is off.
Document documentFromTemplate(const SongTemplate& t);
// A template from a song (Library > Templates > Save this song as template, or a future analysis of a reference track).
// Audio tracks and audio files are left out (a template carries no audio). Throws DocumentError if
// nothing is left.
SongTemplate templateFromDocument(const Document& song, TemplateInfo info);
// A template of a passage of the song (the loop or a section): [startTick, endTick) becomes the whole template,
// cut with deleteTime (clips, automation, sections and tempo changes that cross an edge are cut there; the tempo
// playing at startTick becomes the start tempo). info.source gets {"from": song name, "startTick", "endTick"}.
// Throws DocumentError if the passage is empty or outside the song (Diogo 2026-11-09: examples for the AI).
SongTemplate templateFromRange(const Document& song, TemplateInfo info, int64_t startTick, int64_t endTick);

// A readable id from a name: lower-case letters, digits and '-'.
std::string templateIdFromName(const std::string& name);

struct TemplateEntry {
  SongTemplate tmpl;
  std::filesystem::path file;
  bool builtin = false;     // from the app's folder (read only)
  bool overridesBuiltin = false;  // a user file with the id of a built-in template
};
struct TemplateProblem { std::filesystem::path file; std::string message; };
struct TemplateLibrary {
  std::vector<TemplateEntry> templates;  // built-in ones first, then the user's; each by name
  std::vector<TemplateProblem> problems;  // files that could not be read (left untouched)
  const TemplateEntry* find(const std::string& id) const;
};
// Reads every "*.core-template.json" of both folders (a missing folder is just empty). A user template
// with the id of a built-in one takes its place; built-in ones hidden by deleteTemplate are left out.
// A built-in template marked as a base ("source": {"base": true}) stays in the folder for the generators and the
// tests but is not listed in the app (Diogo 2026-11-07: the 16 old templates leave the app, kept as bases).
// withBases lists them too.
bool isBaseTemplate(const SongTemplate& t);
TemplateLibrary loadTemplates(const std::filesystem::path& builtinFolder, const std::filesystem::path& userFolder,
                              bool withBases = false);
// Writes <userFolder>/<id>.core-template.json safely (the previous one becomes .bak). The version is set to
// one more than the highest the library has for that id, and the template's sheet (templateSheet: every sample
// and parameter by name, value and unit) is written into its "ficha" field. Returns the file written. Throws SaveError.
std::filesystem::path saveTemplate(SongTemplate t, const TemplateLibrary& library, const std::filesystem::path& userFolder);
// Takes a template out of the Library's Templates tab. A user file becomes "<file>.bak" (the previous .bak is replaced); a
// built-in one (or the built-in one a user copy replaced) is hidden by its id in <userFolder>/hidden.json, and
// its file is never touched. Throws SaveError.
void deleteTemplate(const TemplateLibrary& library, const std::string& id, const std::filesystem::path& userFolder);
// Shows every hidden built-in template again; returns how many.
int restoreBuiltinTemplates(const std::filesystem::path& userFolder);

} // namespace core::doc
