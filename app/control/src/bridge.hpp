// The bridge between Core and AICORE: plain files in one folder (the app's: %APPDATA%\Core\AI), no network, no
// pipe, no server. AICORE (the chat, the model, the guides, Diogo's tastes) reads what Core writes and writes
// proposals; Core never runs anything AICORE sends, it only shows a proposal and applies it when Diogo presses Apply.
//   core-state.json      written by Core: the functions it can call and the song (control/calls.hpp, coreState)
//   inbox/<id>.json      written by AICORE: one proposal of calls each
//   outbox/<id>.json     written by Core when a proposal is applied, dismissed or refused: what happened and why
//                        (AICORE learns from it); the inbox file is removed then
//   aicore-status.json   written by AICORE every few seconds while it runs: is its chat open, is it connected to
//                        an AI model (the AICORE button's dot: green only when both, and the file is fresh)
//   aicore-command.json  written by Core: asks an AICORE that is already running to show a page (the chat)
// Every write is atomic (a temporary file, then a rename), so neither side ever reads half a file.
// Contract: docs/contracts/core-calls.md.
#pragma once
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "json.hpp"

namespace core::control {

struct InboxItem {
  std::string id;             // the proposal's "id" when it is a safe name, else the file's name
  std::filesystem::path file;
  nlohmann::json proposal;    // null when the file could not be read (error says why)
  std::string error;
};

// Writes core-state.json (creates the folder). Returns false (and why) when it could not.
bool writeState(const std::filesystem::path& folder, const nlohmann::json& state, std::string* error = nullptr);
// The proposals waiting, oldest first. A file being written (a .tmp) is skipped; a file that is not a JSON object
// is listed with its error, so it can be dismissed.
std::vector<InboxItem> readInbox(const std::filesystem::path& folder);
// The end of a proposal: writes outbox/<id>.json ({id, status: applied | dismissed | refused, at, ...result}) and
// removes the inbox file. Returns false (and why) when the result could not be written (the inbox file stays).
bool finishProposal(const std::filesystem::path& folder, const InboxItem& item, const std::string& status, const nlohmann::json& result,
                    std::string* error = nullptr);
// A cheap signature of the inbox (names, sizes, times), to see if anything changed without reading the files.
std::string inboxSignature(const std::filesystem::path& folder);

// ---------------------------------------------------------------- AICORE, Core's add-on

// What AICORE says about itself (aicore-status.json). "running" only when the file is fresh: AICORE writes it
// every few seconds, so an old file means AICORE was closed (or crashed).
struct AicoreStatus {
  bool running = false;    // the file is there and fresh
  bool chatOpen = false;   // AICORE's chat (Assistant page) is open
  bool connected = false;  // ... and connected to an AI model
  std::string model;       // the model's name, when AICORE says it
  bool ready() const { return running && chatOpen && connected; }  // the green dot
};
constexpr std::chrono::seconds kAicoreStatusMaxAge{10};
// now: the time to compare "updatedAt" (milliseconds since 1970, UTC) with.
AicoreStatus readAicoreStatus(const std::filesystem::path& folder, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
// Asks a running AICORE to show one of its pages ("assistant": the chat). Atomic, like every write here.
bool writeAicoreCommand(const std::filesystem::path& folder, const std::string& page, std::string* error = nullptr);
// AICORE saved new samples into the library (Studio > Edit > Save as sample): it rewrites samples-added.json.
// Returns "<size>:<time>" of that file, "" when there is none; Core rescans the library when it changes.
std::string samplesAddedSignature(const std::filesystem::path& folder);

// The add-on itself: a folder with addon.json ({"name": "AICORE", "version", "minCore", "open": {"program",
// "args"}}). "{folder}" in the program or the args is the add-on's folder; "{page}" the page to open.
struct Addon {
  std::filesystem::path folder;
  std::string name, version, minCore;
  std::string program;
  std::vector<std::string> args;
};
// The add-on in this folder, or nothing (and why) when there is none or its addon.json is not usable.
std::optional<Addon> readAddon(const std::filesystem::path& folder, std::string* error = nullptr);
// The first of these folders that holds a usable add-on (the folder chosen in Settings first, then the one
// installed beside Core, then the checkout on D:).
std::optional<Addon> findAddon(const std::vector<std::filesystem::path>& folders);
// The program's arguments with {folder} and {page} filled in.
std::vector<std::string> addonArgs(const Addon& addon, const std::string& page);
// True when this Core (e.g. "0.9.3") is at least the add-on's minCore (empty minCore: any).
bool addonFits(const Addon& addon, const std::string& coreVersion);

}  // namespace core::control
