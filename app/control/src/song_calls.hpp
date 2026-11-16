// The song's own functions (Diogo, 2026-11-16: the AI inside Core makes beats, the song's structure and changes parts).
// They join the same list as the other calls (calls.hpp), are checked by validateProposal and made by applyProposal,
// so a proposal is still applied as ONE undo step and only when Diogo presses Apply:
//   "track"   a new track for notes, by its part (kick, bass, hats...); later calls of the same proposal name it by
//             its name ("on": "Kick") before it exists
//   "pcore"   P-Core writes a part's notes into a track's pattern (style, part, shape, scale, root, bars, density...)
//   "shape"   S-Core: the song's sections from a song shape ("darkpsycore-64")
//   "arrange" S-Core: places the tracks' patterns over the sections by the style's rules (one track, or every one)
//   "plays"   S-Core: one cell of the grid, a track plays a whole section or rests in it
// Sections are named by their name; a proposal that first sets a shape can name the shape's sections.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "calls.hpp"

namespace core::control::songcalls {

bool isSongFamily(const std::string& family);
void addFunctions(std::vector<CoreFunction>& out, const doc::PCoreCatalog* pcore);

// What the earlier calls of a proposal made: the new tracks (name -> part) and the sections the song will have.
struct Pending {
  std::vector<std::pair<std::string, std::string>> newTracks;
  std::vector<std::string> sections;
  const std::pair<std::string, std::string>* newTrack(const std::string& name) const;  // {name, part}, or nullptr
  std::string section(const std::string& name) const;  // the section's name as the song will have it, "" if none
};
Pending startPending(const doc::Document& song);

// Checks one call of a song function (the track it names is already resolved in `call`); false = left out (the
// reason is in problems).
bool validate(const json& c, const CoreFunction& f, const doc::Document& song, const doc::Track* track, Call& call,
              Pending& pending, std::vector<std::string>& problems);

// The track id of a call: a new track's id once it is made ("new:<name>" -> its id).
std::string resolveTrack(const std::string& trackId, const std::map<std::string, std::string>& made);

// Makes the song calls of p from index i (consecutive P-Core calls with the same layers and bars are written
// together, so the bass leaves the kick free). Returns the lines shown, advances i past what it made.
std::vector<std::string> apply(doc::Document& song, const Proposal& p, size_t& i, std::map<std::string, std::string>& made,
                               const ApplyEnv& env);

std::string describe(const Call& c);

}  // namespace core::control::songcalls
