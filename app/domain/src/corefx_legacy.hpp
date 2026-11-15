// CORE FX replaced the separate inserts saturation, delay, reverb, space, echo, spiral and flux (Diogo,
// 2026-11-08: "passa a ser tudo 1"). Songs, templates and recipes that still name one of them are turned into a
// Core FX with the matching module on, here, in one place:
//  - coreFxFrom: the whole Core FX parameter set for an old effect and its values;
//  - coreFxParamOf: where one old parameter went (for automation lanes, modulators, attachments, F-Core moves),
//    with the function that turns its values into the new parameter's.
// The old effects exist nowhere else: the catalogue, the menus and the engine know only "corefx".
#pragma once
#include <functional>
#include <optional>
#include <string>
#include "document.hpp"

namespace core::doc {

bool isLegacyEffect(const std::string& name);

// The Core FX parameters (every one, by name) that sound like `name` with `params` (missing ones at that
// effect's old defaults). bpm: the song's, for FLUX's rate in Hz (a Core FX rate is a note value).
// mixMoves: the old mix is automated, modulated or an attachment's, so Amount takes it as it is (the dry stays whole);
// otherwise the old mix law is kept exactly (Output, Amount, the module's Return, FX Only).
json coreFxFrom(const std::string& name, const json& params, double bpm = 145, bool mixMoves = false);

struct CoreFxParamMap {
  std::string param;                       // the Core FX parameter
  std::function<double(double)> value;     // an old value -> the new one
  bool octaves = false;                    // the new parameter is in Hz (a modulator's depth is in octaves)
};
// Empty when the old parameter has no Core FX counterpart (its automation is dropped and reported).
std::optional<CoreFxParamMap> coreFxParamOf(const std::string& name, const std::string& param, double bpm = 145);

// An attachment turns its effect on with one parameter. On a Core FX made from an old effect it is that module's
// On: when the attachment ends the module stops receiving and what it holds rings out (the tail finishes), and
// Amount stays at the level the old Mix was switched to. Sets those values in params; returns {param, on value}.
std::pair<std::string, double> coreFxAttachSwitch(const std::string& name, double on, json& params);

// Every old effect of the document (track and master chains, with their automation lanes and modulators)
// becomes a Core FX, keeping its id, bypass and extra keys. Returns how many were turned; notes lists what
// could not follow (a lane of a parameter Core FX does not have).
int convertLegacyEffects(Document& doc, std::vector<std::string>* notes = nullptr);

}  // namespace core::doc
