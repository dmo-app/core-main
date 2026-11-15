#pragma once
// Attachments (Diogo, 2026-11-07): named mixes of filters and effects, kept like patterns ("dás o nome que quiseres",
// "ficam numa biblioteca"), put on notes and on parts of a track: "cada nota pode ter start end", "repartes o mesmo
// beat em 2 e cada um tem start end, e metes o attachment que queres", "podes usar só start ou só end", "aparece um
// plugin, start escolhes o attachment e end outro". The same bars, the same samples, sound different each time.
//
// An attachment lives on its track as one or two effects tagged with its name, silent (Mix 0) except where it is
// put. Every note and clip has three places (the same day: "o middle dá a identidade à nota/section, o end/start dá
// a personalidade à ligação"): the middle is the whole note or clip (its identity); start and end are its edges, on
// top of the middle (the link): a note's first and last third (the end runs on to the next note, so the tail is
// heard), a clip's first and last `edge` ticks. An end and the next start with the same attachment
// join into one stretch (the "link"); two different ones cross over in kAttachRamp.
// Where they sound is ordinary automation of each effect's Mix (and the cutoff for "Open"), rebuilt by
// refreshAttaches from the notes and clips; the other knobs are the user's.
// Saved: effect "attach": {name, recipe, param, on}; note and clip extra "attachStart" / "attachMiddle" /
// "attachEnd": name; clip extra
// "attachEdge": ticks (default one bar, at most half the clip).
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// The library: the starting points an attachment is made from.
struct AttachRecipe {
  const char* id;
  const char* name;  // the menu name, and the attachment's first name
  const char* tip;   // what it does, in plain words
};
const std::vector<AttachRecipe>& attachRecipes();
// The library's folder of a recipe ("Glitch", "Filter", "Space", "Movement", "Colour"), for the picker.
const char* attachRecipeFolder(const std::string& id);

// Your own (Diogo, the same day: "estou a usar fx, filtros, encontro um que gosto, guardo como attachment, e depois
// posso usar no start, end ou middle"): an effect of a track saved with a name, kept in a folder (one JSON file each,
// %APPDATA%\Core\Presets\attachments) and offered in every song beside the library. Its recipe id is "user:<name>".
// A file: {"name": "...", "effects": [{"effect": "filter", "params": {...}, "param": "mix", "on": 0.8}]}.
// From an effect: its knobs as they are; the knob that turns it on is its Mix (or Depth for PULSE, Amount...), and
// its value now is how much it sounds where it is put. Refused for an effect with no such knob (EQ, compressor...).
json attachmentFromEffect(const json& fx, const std::string& name);
// Folders inside the folder are the user's own way to sort them (Diogo: "na biblioteca depois posso estruturar por
// pastas"): each one loaded gets "folder": "Glitch/Mine" (from where it lies; none at the top).
std::vector<json> loadUserAttachments(const std::filesystem::path& folder);  // by folder, then name; bad files skipped
// Saved in its "folder" (if any), and a name "Glitch/My roll" saves My roll in the folder Glitch. Returns the file.
std::filesystem::path saveUserAttachment(const std::filesystem::path& folder, const json& attachment);
// The list the menus and addAttach use (the host loads it from the folder at start and after a save).
void setUserAttachments(std::vector<json> list);
const std::vector<json>& userAttachments();

struct AttachInfo { std::string name, recipe; };
// The attachments of a track, in the order they were made (one entry each, even when it has two effects).
std::vector<AttachInfo> trackAttaches(const Document& doc, const std::string& trackId);
// A new attachment from a recipe, named `name` (empty: the recipe's name; a taken name gets " 2", " 3"...).
// Returns its name. Refused when the effect chain has no room or the recipe is unknown.
std::string addAttach(Document& doc, const std::string& trackId, const std::string& recipe, const std::string& name = "");
// Another name for an attachment (the notes and clips that use it follow). Refused when taken or empty.
void renameAttach(Document& doc, const std::string& trackId, const std::string& from, const std::string& to);
// Its effects go; notes and clips that used it lose it.
void removeAttach(Document& doc, const std::string& trackId, const std::string& name);

enum AttachPlace { kAttachStart = 0, kAttachMiddle = 1, kAttachEnd = 2 };
// What to put on a note or clip, place by place: nullopt = leave that place as it is, "" = take it off.
struct AttachSides { std::optional<std::string> start, middle, end; };
int attachNotes(Pattern& p, const std::set<std::string>& noteIds, const AttachSides& sides);  // how many changed
void attachClip(Track& t, const std::string& clipId, const AttachSides& sides, int64_t edgeTicks = 0);
std::string noteAttach(const Note& n, int place);
std::string clipAttach(const Clip& c, int place);
int64_t clipAttachEdge(const Clip& c);  // the length of its start and end parts, in ticks

// Many at once, like Patterns (Diogo, the same day: "selecionar bars, sections e edit, apply attachments: end, start,
// start end, start end middle", the library to pick from, and "same, alternate, random: same, start e end são iguais;
// alternate usa um attachment e liga a outro diferente; random é caos total"). The notes or clips go in time order;
// link k joins item k's End to item k+1's Start.
//  same:      a link has one attachment on both sides (names taken in turn, link by link): one joined stretch;
//  alternate: each item has its own attachment (in turn) on both edges, so every link goes from one to another
//             (with one name: on, off, on, off);
//  random:    every place draws one of the names, or none (one in four).
// The middle (when asked) takes the names in turn (random: drawn). Only the places asked are written.
struct AttachPlan {
  bool start = false, middle = false, end = true;
  std::vector<std::string> names;   // attachments of the track
  std::string mode = "same";        // same | alternate | random
  uint64_t seed = 1;
};
std::vector<AttachSides> planAttachments(size_t count, const AttachPlan& plan);
int applyAttachPlan(Pattern& p, const std::set<std::string>& noteIds, const AttachPlan& plan);  // notes changed
int applyAttachPlan(Track& t, const std::vector<std::string>& clipIds, const AttachPlan& plan);  // clips changed
// The track's attachment made from that recipe (the first), or a new one with the recipe's name. Returns its name.
std::string ensureAttach(Document& doc, const std::string& trackId, const std::string& recipe);

constexpr int64_t kAttachRamp = kPpq / 16;  // fade in / out of an attachment (a 1/64 note): it never clicks
// The stretches of song time where an attachment of a track sounds, joined and sorted.
std::vector<std::pair<int64_t, int64_t>> attachSpans(const Document& doc, const std::string& trackId, const std::string& name);
// Rewrites the curves of every attachment of every track. Never throws; does nothing on a song without them.
void refreshAttaches(Document& doc);

}  // namespace core::doc
