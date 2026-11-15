// Stage Core (S-Core) (layer 3, structure): the song's parts (intro, build-up, drop, break, roll, outro) and which tracks
// play in each. It has no sounds or patterns of its own: it places the clips of the patterns the tracks already have
// (written by P-Core), puts F-Core's build-up (the RISE curve) on the riser tracks over each build-up, and R-Core /
// D-Core follow the sections by themselves. Every plugin's rules apply: it works on the selected tracks at once (one
// undo step, all or none), then each track and each section can be changed alone; the style guides, never blocks.
//
// The rules (which part plays where) come from CORE-MANUAL template-rules (the dark psycore MVP plan, core-main#148,
// tools/templates/mvp.py) and bonedo.de "Crashkurs Psytrance #4": the drop starts without percussion and adds it
// later, the outro comes down to kick and bass, a drone in the intro and the break, one bar without kick and bass
// before each drop. A hypothesis until heard.
#pragma once
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// A kind of section, from its name ("Drop 2" is a drop; "Build-up", "Build" and "Rise" are build-ups).
// intro | build | drop | break | roll | outro | "" (unknown: S-Core leaves it alone)
std::string scoreStageOf(const std::string& sectionName);

// A song shape: the sections in order, with their length in bars.
struct ScoreStage { const char* name; int bars; };
struct ScoreShape {
  const char* id;     // "darkpsycore-64"
  const char* name;   // "Dark psycore · 64 bars"
  const char* style;  // darkpsycore | psytrance | hitech | slambient
  const char* info;
  std::vector<ScoreStage> stages;
};
const std::vector<ScoreShape>& scoreShapes();
const ScoreShape* findScoreShape(const std::string& id);
// Replaces the song's sections with the shape's, from bar 0 (the clips stay where they are). Lengthens the song to fit.
void setScoreShape(Document& doc, const std::string& shapeId);

// The part S-Core treats a track as: kick, bass, hats, openhats, perc, lead, pad, riser, impact, or "" (unsure:
// such a track plays in every section, so nothing disappears by surprise).
std::string scorePartOf(const Track& track);
const std::vector<std::string>& scoreParts();
std::string scorePartName(const std::string& part);  // "Open hats"

// Where a part plays in a section of a kind, as bars [from, to) counted from the section's start (negative = from its
// end). Empty = it rests there. The bar before a drop (the end of a build-up or a break) has no kick and no bass.
struct ScoreSpan { int from = 0, to = 0; };  // to 0 = the section's end
std::vector<ScoreSpan> scoreRule(const std::string& part, const std::string& stage, bool beforeDrop);

// Does the track play anywhere in the section? (a clip overlaps it)
bool scorePlays(const Track& track, const Section& section);

// Arranges the tracks over the sections by the rules: in each section, the track's clips there go and new ones of its
// pattern go where its part plays (the pattern it played there before, else its first). Tracks with no pattern are
// skipped. Sections of an unknown kind are left alone. Returns how many clips were written.
int scoreArrange(Document& doc, const std::vector<std::string>& trackIds, const std::vector<std::string>& sectionIds = {});
// One cell of the grid: the track plays the whole section (on) or rests in it (off).
void scoreSetPlays(Document& doc, const std::string& trackId, const std::string& sectionId, bool on);
// F-Core's build-up (the RISE curve) over every build-up section, on the tracks given that have F-Core.
int scoreRises(Document& doc, const std::vector<std::string>& trackIds);

}  // namespace core::doc
