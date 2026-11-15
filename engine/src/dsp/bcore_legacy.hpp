// B-Core ("elastic") was removed on 2026-11-09 (Diogo: "o b-core deixa de existir, vão para synth"): Core's bass is
// W-Core. "Nada antigo": no B-Core engine is kept; a song, a template or a project that still has a B-Core track (or
// the older "bass" synth, removed 2026-11-07, which became B-Core) is converted when it is read:
//   - the B-Core preset closest to the track's knobs (each knob scaled to its range; times and Tone on a log scale)
//     gives the sound: W-Core's copy of that preset, found by tools/bcore_fit/fitwave.py against B-Core's own note
//     (A1, log-mel over time), at the same loudness;
//   - Octave carries over as it is; Volume scales the copy's Volume by the track's Volume / the preset's;
//   - automation and modulators of Volume, Octave and Tone follow (Volume and Tone scaled by the copy's / the
//     preset's); the rest of B-Core's knobs have no W-Core twin: their automation and modulators go (load notes).
// Header only: the engine (core-project files) and the document (core-document files) both use it.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/json.hpp"

namespace core::blegacy {

using json = nlohmann::json;

// The old "bass" synth's knobs -> B-Core's (2026-11-07), with the old synth's defaults.
struct OldBassParam { const char* from; const char* to; double def; };
inline const std::vector<OldBassParam>& oldBassParams() {
  static const std::vector<OldBassParam> list = {
      {"octave", "octave", 0},     {"sub", "body", 0.25},          {"cutoff", "tone", 240},   {"resonance", "growl", 0.4},
      {"envAmount", "snap", 4.0},  {"filterDecay", "elastic", 80}, {"attack", "attack", 1},   {"decay", "length", 100},
      {"sustain", "sustain", 0.2}, {"release", "release", 25},     {"drive", "drive", 0.15},  {"level", "level", 0.7}};
  return list;
}
// An old "bass" track's knobs as B-Core knobs (the old synth's saw wave).
inline json oldBassToBCore(const json& old) {
  json params = {{"wave", 3.0}};
  for (const auto& p : oldBassParams())
    params[p.to] = old.is_object() && old.contains(p.from) && old[p.from].is_number() ? old[p.from] : json(p.def);
  return params;
}
// The name an old "bass" knob had in B-Core ("" = none).
inline std::string oldBassName(const std::string& from) {
  for (const auto& p : oldBassParams()) if (from == p.from) return p.to;
  return "";
}

// B-Core's knobs: default, low, high, log scale.
struct Knob { const char* name; double def, lo, hi; bool log; double weight; };
inline const std::vector<Knob>& knobs() {
  static const std::vector<Knob> list = {
      {"wave", 1, 0, 4, false, 1},       {"octave", 0, -2, 2, false, 0},     {"body", 0.6, 0, 1, false, 1},
      {"tone", 120, 30, 16000, true, 2}, {"snap", 4.5, 0, 7, false, 1},      {"elastic", 120, 5, 800, true, 1},
      {"growl", 0.5, 0, 0.95, false, 1}, {"length", 150, 20, 1500, true, 1}, {"drive", 0.35, 0, 1, false, 1},
      {"level", 0.7, 0, 1.5, false, 0},  {"dive", 0, 0, 12, false, 1},       {"attack", 0.5, 0.5, 200, true, 0.5},
      {"sustain", 0, 0, 1, false, 1},    {"release", 20, 2, 2000, true, 0.5}, {"osc2", 0, 0, 1, false, 0.5},
      {"osc2Wave", 3, 0, 4, false, 0.2}, {"osc2Semi", 0, -24, 24, false, 0.2}, {"osc2Fine", 7, -100, 100, false, 0.1},
      {"unison", 0, 0, 1, false, 0.5},   {"filterMode", 0, 0, 19, false, 0.5}, {"noise", 0, 0, 1, false, 0.5},
      {"hollow", 0, 0, 1, false, 0.5},   {"squash", 0, 0, 1, false, 0.5}};
  return list;
}

// B-Core's 15 presets and W-Core's copy of each (layer 1's knobs, then its links).
struct Copy { const char* id; std::map<std::string, double> bcore; std::map<std::string, double> wave; const char* links; };
inline const std::vector<Copy>& copies() {
  static const std::vector<Copy> list = {
      {"core-roller", {{"body", 0.23}, {"dive", 3.69}, {"drive", 0.32}, {"elastic", 24}, {"filterMode", 0}, {"growl", 0.7}, {"length", 189}, {"level", 0.306}, {"noise", 0.01}, {"osc2", 0.05}, {"snap", 5.84}, {"squash", 0.17}, {"sustain", 0.55}, {"tone", 125}, {"unison", 0.24}, {"wave", 3.92}},
       {{"bite", 0.42136}, {"drive", 0.75}, {"env1Attack", 6.12594}, {"env1Decay", 746.228}, {"env1Hold", 198.581}, {"env1Release", 135.939}, {"env1Sustain", 0.43972}, {"env2Attack", 0}, {"env2Decay", 138.755}, {"env2Release", 50}, {"env2Sustain", 0.14961}, {"env3Attack", 0}, {"env3Decay", 19.1802}, {"env3Release", 50}, {"filterDrive", 0.33856}, {"filterMode", 1}, {"filterShape", 0.99993}, {"fm", 0.38664}, {"glide", 0}, {"legato", 0}, {"level", 0.487}, {"mode", 0}, {"noise", 0.29999}, {"octave", 0}, {"osc2", 0.32509}, {"osc2Fine", -27.4352}, {"osc2Semi", 12}, {"osc2Wave", 3.23674}, {"subLevel", 0.56952}, {"subOct", -1}, {"subShape", 1}, {"tone", 44.6452}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.45882}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 6.904487627077376}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 8.043175730059644}]})"},
      {"fullon-rolling", {{"body", 0.1}, {"dive", 3.69}, {"drive", 0.32}, {"elastic", 24}, {"filterMode", 0}, {"growl", 0.7}, {"length", 189}, {"level", 0.231}, {"noise", 0.01}, {"osc2", 0.05}, {"snap", 6.24}, {"squash", 0.7}, {"sustain", 0.55}, {"tone", 200}, {"unison", 0.24}, {"wave", 3.92}},
       {{"bite", 0.10248}, {"drive", 0.88923}, {"env1Attack", 4.44772}, {"env1Decay", 42.9232}, {"env1Hold", 15.9035}, {"env1Release", 144.79}, {"env1Sustain", 0.56659}, {"env2Attack", 0}, {"env2Decay", 202.068}, {"env2Release", 50}, {"env2Sustain", 0.19569}, {"env3Attack", 0}, {"env3Decay", 59.6444}, {"env3Release", 50}, {"filterDrive", 0.65844}, {"filterMode", 9}, {"filterShape", 0.96804}, {"fm", 1.23394}, {"glide", 0}, {"legato", 0}, {"level", 0.5441}, {"mode", 0}, {"noise", 0.29384}, {"octave", 0}, {"osc2", 0.69962}, {"osc2Fine", -9.79874}, {"osc2Semi", 0}, {"osc2Wave", 1.50993}, {"subLevel", 0.86714}, {"subOct", 0}, {"subShape", 0}, {"tone", 261.396}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.50037}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 5.858222058371108}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 18.769810610088722}]})"},
      {"progressive-round", {{"body", 0.4}, {"dive", 3.69}, {"drive", 0.32}, {"elastic", 24}, {"filterMode", 0}, {"growl", 0.4}, {"length", 189}, {"level", 0.245}, {"noise", 0.03}, {"osc2", 0.05}, {"snap", 4.34}, {"squash", 0.6}, {"sustain", 0.55}, {"tone", 125}, {"unison", 0.24}, {"wave", 3.92}},
       {{"bite", 0.12489}, {"drive", 0.30466}, {"env1Attack", 4.80148}, {"env1Decay", 39.0271}, {"env1Hold", 14.4869}, {"env1Release", 164.813}, {"env1Sustain", 0.46131}, {"env2Attack", 0}, {"env2Decay", 141.437}, {"env2Release", 50}, {"env2Sustain", 0.15812}, {"env3Attack", 0}, {"env3Decay", 19.4191}, {"env3Release", 50}, {"filterDrive", 0.50704}, {"filterMode", 12}, {"filterShape", 0.99993}, {"fm", 1.74797}, {"glide", 0}, {"legato", 0}, {"level", 0.8895}, {"mode", 0}, {"noise", 0.29998}, {"octave", 0}, {"osc2", 0.21471}, {"osc2Fine", -3.42402}, {"osc2Semi", 12}, {"osc2Wave", 1.99996}, {"subLevel", 0.21626}, {"subOct", -1}, {"subShape", 0}, {"tone", 205.016}, {"uniWidth", 0}, {"unison", 1}, {"wave", 0.12621}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 5.8898893347492}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 18.19966160474791}]})"},
      {"twin-roller", {{"body", 0.23}, {"dive", 3.69}, {"drive", 0.32}, {"elastic", 24}, {"filterMode", 0}, {"growl", 0.7}, {"length", 189}, {"level", 0.272}, {"noise", 0.01}, {"osc2", 0.45}, {"osc2Fine", 4}, {"osc2Semi", 12}, {"osc2Wave", 2}, {"snap", 5.84}, {"squash", 0.17}, {"sustain", 0.55}, {"tone", 125}, {"unison", 0.24}, {"wave", 3.92}},
       {{"bite", 0.37885}, {"drive", 0.37354}, {"env1Attack", 2.89481}, {"env1Decay", 2114.75}, {"env1Hold", 143.454}, {"env1Release", 140.104}, {"env1Sustain", 0.40361}, {"env2Attack", 0}, {"env2Decay", 157.891}, {"env2Release", 50}, {"env2Sustain", 0.044}, {"env3Attack", 0}, {"env3Decay", 66.4246}, {"env3Release", 50}, {"filterDrive", 0.03057}, {"filterMode", 12}, {"filterShape", 0.99993}, {"fm", 0.48578}, {"glide", 0}, {"legato", 0}, {"level", 0.5268}, {"mode", 0}, {"noise", 0.24734}, {"octave", 0}, {"osc2", 0.78395}, {"osc2Fine", -8.98311}, {"osc2Semi", 12}, {"osc2Wave", 0.78034}, {"subLevel", 0.32}, {"subOct", 0}, {"subShape", 0}, {"tone", 277.644}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.42662}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 5.065661571498718}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 17.5536568076405}]})"},
      {"hitech-snap", {{"body", 0.22}, {"dive", 1.09}, {"drive", 0.02}, {"elastic", 24}, {"filterMode", 1}, {"growl", 0.62}, {"length", 72}, {"level", 0.246}, {"noise", 0.24}, {"osc2", 0.36}, {"snap", 5.99}, {"squash", 0.37}, {"sustain", 0.55}, {"tone", 91}, {"unison", 0.35}, {"wave", 3.9}},
       {{"bite", 0.21459}, {"drive", 0.59924}, {"env1Attack", 4.76019}, {"env1Decay", 111.909}, {"env1Hold", 3.32221}, {"env1Release", 165.221}, {"env1Sustain", 0.32786}, {"env2Attack", 0}, {"env2Decay", 143.173}, {"env2Release", 50}, {"env2Sustain", 0.18938}, {"env3Attack", 0}, {"env3Decay", 10.8576}, {"env3Release", 50}, {"filterDrive", 0.17811}, {"filterMode", 12}, {"filterShape", 0.99993}, {"fm", 0.15515}, {"glide", 0}, {"legato", 0}, {"level", 0.7208}, {"mode", 0}, {"noise", 0.0572}, {"octave", 0}, {"osc2", 0.42516}, {"osc2Fine", -12.2455}, {"osc2Semi", 12}, {"osc2Wave", 2.00022}, {"subLevel", 0.31117}, {"subOct", 0}, {"subShape", 0}, {"tone", 79.868}, {"uniWidth", 0}, {"unison", 1}, {"wave", 1.40526}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 6.8866414633615305}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 1.1454526134384844}]})"},
      {"laser-rubber", {{"body", 0.22}, {"dive", 4.5}, {"drive", 0.5}, {"elastic", 24}, {"filterMode", 1}, {"growl", 0.5}, {"length", 72}, {"level", 0.215}, {"noise", 0.24}, {"osc2", 0.36}, {"snap", 5.99}, {"squash", 0.37}, {"sustain", 0.55}, {"tone", 91}, {"unison", 0.35}, {"wave", 4}},
       {{"bite", 0.50191}, {"drive", 0.22076}, {"env1Attack", 1.67252}, {"env1Decay", 47.0705}, {"env1Hold", 16.4428}, {"env1Release", 127.772}, {"env1Sustain", 0.21768}, {"env2Attack", 0}, {"env2Decay", 235.724}, {"env2Release", 50}, {"env2Sustain", 0.37362}, {"env3Attack", 0}, {"env3Decay", 21.9365}, {"env3Release", 50}, {"filterDrive", 0.95699}, {"filterMode", 1}, {"filterShape", 0.99967}, {"fm", 1.03948}, {"glide", 0}, {"legato", 0}, {"level", 1.1534}, {"mode", 0}, {"noise", 0.27346}, {"octave", 0}, {"osc2", 0.49788}, {"osc2Fine", -19.3559}, {"osc2Semi", 12}, {"osc2Wave", 2.95872}, {"subLevel", 0.55112}, {"subOct", -1}, {"subShape", 1}, {"tone", 126.544}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.18664}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 3.8042360192367757}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 9.016826556642272}]})"},
      {"saw-stack", {{"body", 0.22}, {"dive", 1.09}, {"drive", 0.02}, {"elastic", 24}, {"filterMode", 1}, {"growl", 0.62}, {"length", 72}, {"level", 0.234}, {"noise", 0.24}, {"osc2", 0.2}, {"snap", 5.99}, {"squash", 0.5}, {"sustain", 0.55}, {"tone", 127}, {"unison", 0.6}, {"wave", 3.9}},
       {{"bite", 0.19915}, {"drive", 0.82783}, {"env1Attack", 1.13586}, {"env1Decay", 214.688}, {"env1Hold", 21.1722}, {"env1Release", 169.803}, {"env1Sustain", 0.29168}, {"env2Attack", 0}, {"env2Decay", 130.595}, {"env2Release", 50}, {"env2Sustain", 0.09693}, {"env3Attack", 0}, {"env3Decay", 169.945}, {"env3Release", 50}, {"filterDrive", 0.53506}, {"filterMode", 12}, {"filterShape", 0.99993}, {"fm", 0.4964}, {"glide", 0}, {"legato", 0}, {"level", 0.5088}, {"mode", 0}, {"noise", 1e-05}, {"octave", 0}, {"osc2", 0.70725}, {"osc2Fine", 15.7947}, {"osc2Semi", 12}, {"osc2Wave", 1.99509}, {"subLevel", 0.99994}, {"subOct", 0}, {"subShape", 0}, {"tone", 151.928}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.84213}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 6.127133648226074}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 2.7197905844201116}]})"},
      {"darkpsy-plonk", {{"body", 0.22}, {"dive", 1.96}, {"drive", 0.61}, {"elastic", 23}, {"filterMode", 1}, {"growl", 0.69}, {"length", 253}, {"level", 0.199}, {"noise", 0.09}, {"osc2", 0.03}, {"snap", 5.31}, {"squash", 0.85}, {"sustain", 0.55}, {"tone", 51}, {"unison", 0.42}, {"wave", 3.89}},
       {{"bite", 0.22747}, {"drive", 0.40636}, {"env1Attack", 5.59264}, {"env1Decay", 24.3934}, {"env1Hold", 16.1264}, {"env1Release", 165.216}, {"env1Sustain", 0.40684}, {"env2Attack", 0}, {"env2Decay", 87.6393}, {"env2Release", 50}, {"env2Sustain", 0.15107}, {"env3Attack", 0}, {"env3Decay", 18.0036}, {"env3Release", 50}, {"filterDrive", 0.4024}, {"filterMode", 12}, {"filterShape", 0.99993}, {"fm", 0.69242}, {"glide", 0}, {"legato", 0}, {"level", 0.8686}, {"mode", 0}, {"noise", 0.29399}, {"octave", 0}, {"osc2", 0.2902}, {"osc2Fine", 2.94322}, {"osc2Semi", 0}, {"osc2Wave", 3.11468}, {"subLevel", 0.08389}, {"subOct", -1}, {"subShape", 1}, {"tone", 460.323}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.71931}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 5.079425564609887}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 14.755279908620405}]})"},
      {"rubber-growl", {{"body", 0.22}, {"dive", 2}, {"drive", 0.61}, {"elastic", 55}, {"filterMode", 1}, {"growl", 0.85}, {"length", 253}, {"level", 0.167}, {"noise", 0.09}, {"osc2", 0.03}, {"snap", 5.31}, {"squash", 0.85}, {"sustain", 0.55}, {"tone", 51}, {"unison", 0.42}, {"wave", 3.3}},
       {{"bite", 0.14029}, {"drive", 0.83882}, {"env1Attack", 15.8311}, {"env1Decay", 20.1687}, {"env1Hold", 52.7031}, {"env1Release", 161.688}, {"env1Sustain", 0.47355}, {"env2Attack", 0}, {"env2Decay", 171.008}, {"env2Release", 50}, {"env2Sustain", 0.17674}, {"env3Attack", 0}, {"env3Decay", 15.1998}, {"env3Release", 50}, {"filterDrive", 0.12224}, {"filterMode", 12}, {"filterShape", 0.99993}, {"fm", 1.5107}, {"glide", 0}, {"legato", 0}, {"level", 0.4877}, {"mode", 0}, {"noise", 2e-05}, {"octave", 0}, {"osc2", 0.41787}, {"osc2Fine", 4.63267}, {"osc2Semi", 12}, {"osc2Wave", 1.93212}, {"subLevel", 0.20929}, {"subOct", -1}, {"subShape", 1}, {"tone", 426.925}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.67372}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 6.173031819060368}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 14.68309229391861}]})"},
      {"deep-plonk", {{"body", 0.45}, {"dive", 2.5}, {"drive", 0.61}, {"elastic", 23}, {"filterMode", 1}, {"growl", 0.69}, {"length", 253}, {"level", 0.212}, {"noise", 0.09}, {"osc2", 0.03}, {"snap", 4.11}, {"squash", 0.85}, {"sustain", 0.55}, {"tone", 36}, {"unison", 0.42}, {"wave", 3.89}},
       {{"bite", 0.47123}, {"drive", 0.84894}, {"env1Attack", 2.96702}, {"env1Decay", 20}, {"env1Hold", 11.797}, {"env1Release", 163.879}, {"env1Sustain", 0.37811}, {"env2Attack", 0}, {"env2Decay", 163.265}, {"env2Release", 50}, {"env2Sustain", 0.10945}, {"env3Attack", 0}, {"env3Decay", 13.6376}, {"env3Release", 50}, {"filterDrive", 0.3364}, {"filterMode", 1}, {"filterShape", 0.99993}, {"fm", 1.87773}, {"glide", 0}, {"legato", 0}, {"level", 0.7285}, {"mode", 0}, {"noise", 0.25853}, {"octave", 0}, {"osc2", 0.40199}, {"osc2Fine", 6.81383}, {"osc2Semi", 12}, {"osc2Wave", 0.99994}, {"subLevel", 0.6837}, {"subOct", 0}, {"subShape", 0}, {"tone", 50.7959}, {"uniWidth", 0}, {"unison", 1}, {"wave", 1.30654}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 5.118447519234637}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 7.317431835410632}]})"},
      {"psycore-plonk", {{"body", 0.22}, {"dive", 1.96}, {"drive", 0.61}, {"elastic", 16}, {"filterMode", 1}, {"growl", 0.69}, {"length", 90}, {"level", 0.214}, {"noise", 0.09}, {"osc2", 0.03}, {"release", 8}, {"snap", 5.31}, {"squash", 0.85}, {"sustain", 0.55}, {"tone", 51}, {"unison", 0.42}, {"wave", 3.89}},
       {{"bite", 0.38938}, {"drive", 0.49943}, {"env1Attack", 6.43231}, {"env1Decay", 2952.83}, {"env1Hold", 178.613}, {"env1Release", 58.5663}, {"env1Sustain", 0.98864}, {"env2Attack", 0}, {"env2Decay", 73.2237}, {"env2Release", 50}, {"env2Sustain", 0.03}, {"env3Attack", 0}, {"env3Decay", 51.0311}, {"env3Release", 50}, {"filterDrive", 0.92422}, {"filterMode", 1}, {"filterShape", 0.97837}, {"fm", 0.60585}, {"glide", 0}, {"legato", 0}, {"level", 0.4733}, {"mode", 0}, {"noise", 0.12621}, {"octave", 0}, {"osc2", 0.2604}, {"osc2Fine", 4.969}, {"osc2Semi", 12}, {"osc2Wave", 3.39703}, {"subLevel", 0.16779}, {"subOct", -1}, {"subShape", 0}, {"tone", 467.553}, {"uniWidth", 0}, {"unison", 1}, {"wave", 0.21113}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 6.2049153439202795}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 8.859721125368715}]})"},
      {"psycore-goblin", {{"body", 0.22}, {"dive", 1.96}, {"drive", 0.61}, {"elastic", 22}, {"filterMode", 1}, {"growl", 0.85}, {"length", 90}, {"level", 0.188}, {"noise", 0.09}, {"osc2", 0.03}, {"release", 8}, {"snap", 5.31}, {"squash", 0.85}, {"sustain", 0.55}, {"tone", 51}, {"unison", 0.42}, {"wave", 3.3}},
       {{"bite", 0.73768}, {"drive", 0.42873}, {"env1Attack", 13.2233}, {"env1Decay", 23.06}, {"env1Hold", 9.11628}, {"env1Release", 67.1184}, {"env1Sustain", 0.32504}, {"env2Attack", 0}, {"env2Decay", 115.01}, {"env2Release", 50}, {"env2Sustain", 0.46074}, {"env3Attack", 0}, {"env3Decay", 10.2446}, {"env3Release", 50}, {"filterDrive", 0.29357}, {"filterMode", 1}, {"filterShape", 0.99993}, {"fm", 1.83425}, {"glide", 0}, {"legato", 0}, {"level", 0.7791}, {"mode", 0}, {"noise", 0.29998}, {"octave", 0}, {"osc2", 0.31865}, {"osc2Fine", 11.2415}, {"osc2Semi", 0}, {"osc2Wave", 2.52262}, {"subLevel", 0.39493}, {"subOct", 0}, {"subShape", 1}, {"tone", 267.473}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.52772}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 1.5162097948046849}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 9.139051113238601}]})"},
      {"deep-reese", {{"attack", 5}, {"body", 0.22}, {"dive", 0}, {"drive", 0.61}, {"elastic", 300}, {"filterMode", 1}, {"growl", 0.69}, {"length", 600}, {"level", 0.168}, {"noise", 0.09}, {"osc2", 1}, {"osc2Fine", 18}, {"osc2Semi", 0}, {"osc2Wave", 3}, {"release", 150}, {"snap", 1.5}, {"squash", 0.85}, {"sustain", 0.8}, {"tone", 180}, {"unison", 0.4}, {"wave", 3.89}},
       {{"bite", 0.1881}, {"drive", 0.40462}, {"env1Attack", 41.2191}, {"env1Decay", 995.627}, {"env1Hold", 199.987}, {"env1Release", 399.86}, {"env1Sustain", 0.01999}, {"env2Attack", 0}, {"env2Decay", 1999.21}, {"env2Release", 50}, {"env2Sustain", 7e-05}, {"env3Attack", 0}, {"env3Decay", 189.132}, {"env3Release", 50}, {"filterDrive", 0.0231}, {"filterMode", 1}, {"filterShape", 0.99993}, {"fm", 0.60746}, {"glide", 0}, {"legato", 0}, {"level", 0.4026}, {"mode", 0}, {"noise", 0.29998}, {"octave", 0}, {"osc2", 0.42364}, {"osc2Fine", 8.35265}, {"osc2Semi", 12}, {"osc2Wave", 2.00001}, {"subLevel", 0.8014}, {"subOct", 0}, {"subShape", 0}, {"tone", 417.588}, {"uniWidth", 0}, {"unison", 1}, {"wave", 1.56115}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 3.9727056433433185}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 14.285027614290092}]})"},
      {"slambient-deep", {{"body", 0.69}, {"dive", 2.33}, {"drive", 0.2}, {"elastic", 300}, {"growl", 0.26}, {"length", 900}, {"level", 0.256}, {"osc2", 0.35}, {"release", 300}, {"snap", 2.5}, {"squash", 0.3}, {"sustain", 0.7}, {"tone", 75}, {"unison", 0.58}, {"wave", 2.74}},
       {{"bite", 0.50745}, {"drive", 0.31289}, {"env1Attack", 6.0717}, {"env1Decay", 1401.01}, {"env1Hold", 124.725}, {"env1Release", 399.86}, {"env1Sustain", 7e-05}, {"env2Attack", 0}, {"env2Decay", 1554.71}, {"env2Release", 50}, {"env2Sustain", 0.15506}, {"env3Attack", 0}, {"env3Decay", 68.7188}, {"env3Release", 50}, {"filterDrive", 0.52671}, {"filterMode", 1}, {"filterShape", 0.99993}, {"fm", 1.02139}, {"glide", 0}, {"legato", 0}, {"level", 0.6064}, {"mode", 0}, {"noise", 0.29998}, {"octave", 0}, {"osc2", 0.44379}, {"osc2Fine", 9.6825}, {"osc2Semi", 12}, {"osc2Wave", 0.99992}, {"subLevel", 0.01663}, {"subOct", 0}, {"subShape", 1}, {"tone", 43.8909}, {"uniWidth", 0}, {"unison", 1}, {"wave", 0.95146}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 3.5967189100481654}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 18.025307022197087}]})"},
      {"slow-tide", {{"body", 0.69}, {"dive", 2.33}, {"drive", 0.81}, {"elastic", 600}, {"growl", 0.75}, {"length", 1500}, {"level", 0.217}, {"osc2", 0.35}, {"release", 500}, {"snap", 3.5}, {"squash", 0.4}, {"sustain", 0.6}, {"tone", 70}, {"unison", 0.3}, {"wave", 3}},
       {{"bite", 0.14571}, {"drive", 0.56566}, {"env1Attack", 5.42778}, {"env1Decay", 2570.75}, {"env1Hold", 199.987}, {"env1Release", 399.977}, {"env1Sustain", 0.51445}, {"env2Attack", 0}, {"env2Decay", 1918.36}, {"env2Release", 50}, {"env2Sustain", 0.16885}, {"env3Attack", 0}, {"env3Decay", 16.485}, {"env3Release", 50}, {"filterDrive", 0.00758}, {"filterMode", 9}, {"filterShape", 0.99993}, {"fm", 1.0723}, {"glide", 0}, {"legato", 0}, {"level", 0.5099}, {"mode", 0}, {"noise", 0.0002}, {"octave", 0}, {"osc2", 0.08504}, {"osc2Fine", -10.3301}, {"osc2Semi", 7}, {"osc2Wave", 3.99974}, {"subLevel", 0.99994}, {"subOct", 0}, {"subShape", 0}, {"tone", 75.6757}, {"uniWidth", 0}, {"unison", 1}, {"wave", 3.15312}},
       R"({"links": [{"id": "m1", "source": "env2", "target": "tone", "amount": 6.728914616175764}, {"id": "m2", "source": "env3", "target": "pitch", "amount": 12.546555352498304}]})"}};
  return list;
}

struct Converted {
  json params;              // W-Core's knobs (layer 1)
  json synth;               // its links: {"layers": [layer 1]}
  std::string from;         // the B-Core preset it was closest to
  double levelScale = 1;    // W-Core Volume per B-Core Volume (for Volume's automation and modulators)
  double toneScale = 1;     // W-Core Tone per B-Core Tone (the filter's rest, Hz)
};

// A B-Core track's knobs (missing = B-Core's default) as W-Core.
inline Converted convert(const json& old) {
  auto value = [&](const Knob& k) {
    const double v = old.is_object() && old.contains(k.name) && old[k.name].is_number() ? old[k.name].get<double>() : k.def;
    return std::clamp(v, k.lo, k.hi);
  };
  auto scaled = [](const Knob& k, double v) {
    return k.log ? std::log(std::max(v, k.lo) / k.lo) / std::log(k.hi / k.lo) : (v - k.lo) / (k.hi - k.lo);
  };
  const Copy* best = nullptr;
  double bestD = 1e300;
  for (const auto& c : copies()) {
    double d = 0;
    for (const auto& k : knobs()) {
      const auto it = c.bcore.find(k.name);
      const double a = scaled(k, value(k)), b = scaled(k, it == c.bcore.end() ? k.def : it->second);
      d += k.weight * (a - b) * (a - b);
    }
    if (d < bestD) { bestD = d; best = &c; }
  }
  Converted out;
  out.from = best->id;
  out.params = json::object();
  for (const auto& [k, v] : best->wave) out.params[k] = v;
  const auto presetLevel = best->bcore.count("level") ? best->bcore.at("level") : 0.7;
  const double copyLevel = best->wave.count("level") ? best->wave.at("level") : 0.7;
  out.levelScale = copyLevel / std::max(1e-6, presetLevel);
  const double presetTone = best->bcore.count("tone") ? best->bcore.at("tone") : 120;
  out.toneScale = (best->wave.count("tone") ? best->wave.at("tone") : 1800) / std::max(1e-6, presetTone);
  for (const auto& k : knobs())
    if (std::string(k.name) == "level") out.params["level"] = std::clamp(value(k) * out.levelScale, 0.0, 1.5);
    else if (std::string(k.name) == "octave") out.params["octave"] = std::round(value(k));
  out.synth = {{"layers", json::array({json::parse(best->links)})}};
  return out;
}

// What a B-Core knob's automation or modulator becomes: its W-Core name and the scale of its values ("" = none).
inline std::pair<std::string, double> renameParam(const std::string& name, const Converted& c) {
  if (name == "level") return {"level", c.levelScale};
  if (name == "tone") return {"tone", c.toneScale};
  if (name == "octave") return {"octave", 1};
  return {"", 1};
}

}  // namespace core::blegacy
