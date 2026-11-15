// Project assets (M3): audio files copied into the project folder, identified by their content.
// Plan 3.2 / M3: importing copies by default; a missing file keeps its clips and can be relocated;
// "save as" elsewhere takes the files along, so a project folder is portable.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

constexpr const char* kAssetFolder = "Audio";  // inside the project folder

struct WavInfo { double sampleRate = 0; int channels = 0; int64_t frames = 0; int bits = 0; bool isFloat = false; int64_t dataOffset = 0; };
// The format of a WAV file (PCM 16/24/32, float 32/64, mono or stereo); throws DocumentError otherwise.
WavInfo readWavInfo(const std::filesystem::path& file);

// What a WAV file says about itself in its other chunks (read only; Core never writes them):
//   info  the RIFF LIST INFO texts by id ("ISBJ" subject, "IKEY" keywords, "IART", "ICOP", "IGNR", "INAM"...), UTF-8
//   loop  the first forward loop of its "smpl" chunk, in frames (loopEnd 0: none)
//   acid  an ACID chunk: its tempo (0: none), whether it is a one-shot, its root note (-1: not set)
// A broken or missing chunk is simply left out: never an error (the audio is what matters).
struct WavTags {
  std::map<std::string, std::string> info;
  int64_t loopStart = 0, loopEnd = 0;
  bool hasAcid = false, acidOneShot = false;
  double acidBpm = 0;
  int acidRootKey = -1;
  bool empty() const { return info.empty() && loopEnd <= 0 && !hasAcid; }
};
WavTags readWavTags(const std::filesystem::path& file);

// Copies an audio file into <projectDir>/Audio (named by its hash, so the same file is stored once)
// and adds it to the document. Returns the asset id (an existing one if this content is already there).
std::string importAudio(Document& doc, const std::filesystem::path& file, const std::filesystem::path& projectDir);

// Assets whose file is missing (or has other content) in projectDir.
std::vector<std::string> missingAssets(const Document& doc, const std::filesystem::path& projectDir);
// Points a missing asset at another file: it is copied into the project if its content is the same.
// Throws DocumentError if the content differs (a different take would silently change the song).
void relocateAsset(Document& doc, const std::string& assetId, const std::filesystem::path& file, const std::filesystem::path& projectDir);
// Copies the assets that are not yet in toDir (for "save as" in another folder). Returns how many.
int copyAssets(const Document& doc, const std::filesystem::path& fromDir, const std::filesystem::path& toDir);

std::filesystem::path assetFile(const Asset& asset, const std::filesystem::path& projectDir);

// ---- the project's audio files, for the Library's Project view

// Records that Core made asset `made` from asset `from` (what: "Reverse", "Stretch x1.25"...). Nothing when it is
// the same asset, or when it already has an origin (the same audio made twice keeps its first).
void markMadeFrom(Document& doc, const std::string& made, const std::string& from, const std::string& what);
// How many audio clips play an asset.
int clipsUsingAsset(const Document& doc, const std::string& assetId);
// The assets no audio clip plays (copies left behind by edits, imports whose clips were deleted). A sample saved
// to the song (saveProjectSample) is never unused: it is kept for the song even with no clip.
std::vector<std::string> unusedAssets(const Document& doc);
// "Save as sample > This project": the file becomes one of the song's audio files (copied in like an import, by
// content), named `name` (".wav" added), kept with no clip (extra.sample = true) and, when `fromAsset` is one of the
// song's assets, made from it (`what`: "Sample", "Render"...). Returns the asset id. One undo step for the caller.
std::string saveProjectSample(Document& doc, const std::filesystem::path& file, const std::filesystem::path& projectDir,
                              const std::string& name, const std::string& fromAsset = "", const std::string& what = "Sample");
// True for an asset saveProjectSample kept.
bool isProjectSample(const Asset& asset);
// Takes them out of the document (one undo step for the caller); returns them, so their files can go.
// A file another asset still points at is not returned.
std::vector<Asset> removeUnusedAssets(Document& doc);
// Audio files in <projectDir>/Audio that no asset of the document points at (left by an older version of
// the song, or by an edit that was undone and saved over).
std::vector<std::filesystem::path> strayAudioFiles(const Document& doc, const std::filesystem::path& projectDir);
class History;
// Codex review 2026-11-09 C4: Remove unused sent the files to the Recycle Bin, and Undo then brought a clip back with
// no audio. Of `files`, the ones no state of the history (current, Undo, Redo) points at: only those may go. The
// others stay until the song is closed; Remove unused finds them as stray files next time.
std::vector<std::filesystem::path> filesNoStateNeeds(const History& history, const std::vector<std::filesystem::path>& files,
                                                     const std::filesystem::path& projectDir);

} // namespace core::doc
