#include "wcore.hpp"
#include "corefx_legacy.hpp"
#include <cctype>
#include <fstream>
#include <algorithm>
#include <cmath>
#include "edit.hpp"
#include "fx_racks.hpp"
#include "fcore.hpp"
#include "../../../engine/src/dsp/wave_legacy.hpp"
#include "../../../engine/src/dsp/wave_mod.hpp"

namespace {
namespace fs = std::filesystem;
// Paths and names are UTF-8 in the document; on Windows a plain std::string path would be read as the ANSI code page.
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string toUtf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}
}  // namespace

namespace core::doc {

const std::vector<SoundLayer>& waveLayers() {
  static const std::vector<SoundLayer> layers = {
      {"wave", "Wave", "What each note is made of. Pick a shape by name or move between them freely.",
       {{"wave", "Shape", "Round is soft and pure, Warm a little fuller, Hollow like a clarinet, Buzzy bright and rich "
                          "(the classic lead), Hard thin and nasal. Between two names you hear a blend of both."},
        {"unison", "Unison", "How many copies of the wave play at once, detuned and spread left and right: 1 = one wave "
                             "in the middle (thin, focused), 7 = the classic \"supersaw\", up to 16."},
        {"uniDetune", "Detune", "How far apart the copies are tuned. More = wider, thicker, then a sour chorus."},
        {"uniBlend", "Blend", "How loud the side copies are against the one in the middle."},
        {"uniWidth", "Width", "How far left and right the copies go. 0 = all in the middle (mono)."},
        {"octave", "Octave", "Plays every note an octave lower or higher."},
        {"osc1Semi", "Interval", "Moves Osc 1 by semitones (Osc 2 has its own): 7 = a fifth up, -12 = an octave down."},
        {"tune", "Fine tune", "Moves the pitch a little, up to a semitone (100 cents) either way: to sit with a sample "
                              "or another synth, or a slightly sour, unsettling lead."},
        {"osc1Level", "Level", "How loud Osc 1 is (the others keep theirs). 0 = silent: only Osc 2, the Sub, the noise."}}},
      {"sub", "Sub", "One plain wave under the others, an octave or more down, with no unison: the weight under a lead or a pad.",
       {{"subLevel", "Sub", "How loud the sub is. 0 = off."},
        {"subShape", "Shape", "Osc 1 (the same wave as Osc 1), Sine, Triangle, Saw, Square or Pulse."},
        {"subOct", "Octave", "How far under: -1 = an octave, -2 = two, -3 = three; 0 = the note itself, +1 = above."},
        {"subPan", "Pan", "Where the sub sits, left or right. 0 = the middle (the place for a bass)."}}},
      {"colour", "Colour", "What is added to the wave: noise (air, hiss, wind) and FM (metallic, bell-like, bubbly tones).",
       {{"noise", "Noise", "Mixes noise into the sound, before the filter: air on a pad, the hiss of a riser. 0 = none."},
        {"noiseColor", "Colour", "How bright the noise is. Low = a dark rumble, high = bright hiss."},
        {"fm", "FM", "Makes the wave wobble very fast at another pitch: metallic, glassy or bubbly. 0 = the plain wave."},
        {"fmRatio", "Ratio", "The pitch of that wobble against the note. Whole numbers (1, 2, 3) sound musical, "
                             "in-between numbers (1.41, 3.5) sound like metal and bells."}}},
      {"grain", "Grain", "Cuts a sound into tiny pieces (grains) and plays many at once: clouds, shimmer, frozen textures. "
                         "It uses the sound you pick in Grain sound (a sample of the library), or the wave itself without one.",
       {{"grain", "Grain", "How much of the sound is grains. 0 = the plain wave, full = only grains."},
        {"grainSize", "Size", "How long each grain is. Short (5 to 30 ms) = buzzy, glitchy; long (100 ms and more) = smooth, "
                              "you hear the sound inside."},
        {"grainDensity", "Density", "How many grains start each second. Few = separate drops, many = a dense cloud."},
        {"grainPos", "Position", "Where in the sound the grains start: 0 = its beginning, full = its end."},
        {"grainScan", "Scan", "How fast that place moves through the sound while a note holds. 0 = frozen on one spot, "
                              "1 = at the sound's own speed."},
        {"grainScatter", "Scatter", "How random each grain is: its place, pitch (up to a fifth) and left or right. "
                                    "0 = all alike, full = a wide shimmering cloud."},
        {"grainPitch", "Pitch", "Moves the grains' pitch, in semitones: 12 = an octave up, a shimmer over the note."}}},
      {"filter", "Filter", "How dark or open the sound is. ENV 2 opens it on each note (its link to Tone).",
       {{"tone", "Tone", "Where the filter rests. Low = dark and muffled, high = open and bright."},
        {"bite", "Bite", "How much it screams around Tone. Near the top it whistles like an acid line."},
        {"filterMix", "Mix", "How much of the filtered sound you hear, the rest dry. Full = only the filter."}}},
      {"time", "ENV 1", "The volume of each note: how it starts, holds and ends. Sustain low = a pluck, Sustain full = a "
       "held lead or pad.",
       {{"env1Attack", "Attack", "How long the note takes to fade in. Short = a lead that hits at once, seconds = a pad that grows."},
        {"env1Hold", "Hold", "How long it stays at the top before it falls."},
        {"env1Decay", "Decay", "After the top, how long the note takes to fall to the Sustain level."},
        {"env1Sustain", "Sustain", "How loud the note stays while you hold the key. 0 = a pluck that dies away by itself; "
                                   "full = it holds as long as the key."},
        {"env1Release", "Release", "How long the note lingers after you let go. Long = notes melt into each other."}}},
      {"env2", "ENV 2", "A second envelope on each note, for whatever it is linked to: by default it opens the filter.",
       {{"env2Attack", "Attack", "How long it takes to rise. 0 = at once."},
        {"env2Hold", "Hold", "How long it stays at the top."},
        {"env2Decay", "Decay", "How long it takes to fall to Sustain. Short = a pluck, long = a slow opening."},
        {"env2Sustain", "Sustain", "Where it stays while the key is held."},
        {"env2Release", "Release", "How long it takes to fall after you let go."}}},
      {"env3", "ENV 3", "A third envelope on each note: a zap of pitch, a swell of FM, anything it is linked to.",
       {{"env3Attack", "Attack", "How long it takes to rise. 0 = at once."},
        {"env3Hold", "Hold", "How long it stays at the top."},
        {"env3Decay", "Decay", "How long it takes to fall to Sustain."},
        {"env3Sustain", "Sustain", "Where it stays while the key is held."},
        {"env3Release", "Release", "How long it takes to fall after you let go."}}},
      {"motion", "Motion", "How Lead mode goes from note to note. The slow drift is LFO 3.",
       {{"glide", "Glide", "The note slides into the next one, in Lead mode, when the notes touch. 0 = no slide."},
        {"legato", "Legato", "On: a note played over another does not start again (no new attack), it only changes pitch."},
        {"glideAlways", "Always", "On: Glide slides into every note, also when the notes do not touch."}}},
      // The second page ("More"): the pieces of a Surge XT or Phase Plant patch, so a sound you hear can be rebuilt.
      {"osc2", "Osc 2", "A second wave under the first one, with its own shape and pitch: two saws a fifth apart, an "
       "octave on top, a slightly detuned copy that beats against the first (the reese). It follows Unison and the movements.",
       {{"osc2", "Osc 2", "How loud the second wave is. 0 = only the first wave (as before)."},
        {"osc2Wave", "Shape", "Its shape, like the first one: Round, Warm, Hollow, Buzzy, Hard, or between them."},
        {"osc2Semi", "Interval", "How many semitones above or below the first wave: 7 = a fifth, 12 = an octave, 0 = the same note."},
        {"osc2Fine", "Detune", "A little out of tune with the first wave, in cents. A few cents = a slow beating, thick "
         "sound; 20 or more = a wide, tearing sound."}}},
      {"tables", "Tables", "Real wavetables for each oscillator, as in Surge or Phase Plant: pick a table, then move "
       "through it with Position (the sound morphs from one end to the other). Classic = the five shapes on the main page.",
       {{"osc1Table", "Osc 1", "Osc 1's table: Classic (the Shape knob), PWM, Sync, Vowel, Organ, FM, Fold, Growl, Digital."},
        {"osc1Pos", "Position", "Where in Osc 1's table: each table changes from one end to the other (a pulse narrows, a "
         "sync sweeps up, a vowel goes A-E-I-O-U...)."},
        {"osc2Table", "Osc 2", "Osc 2's table (Classic = Osc 2's own Shape)."},
        {"osc2Pos", "Position", "Where in Osc 2's table."}}},
      {"warp", "Warp", "Bends each oscillator's wave into another one, as in Serum: Sync (the sweeping hard-sync scream), "
       "Bend+ / Bend- (the wave pushed early or late), PWM (squeezed, the rest silent), Asym, Mirror, Flip, Quantize (steps), "
       "FM / AM / Ring from the other oscillator.",
       {{"osc1Warp", "Osc 1", "Osc 1's warp (off = the plain wave)."},
        {"osc1WarpAmt", "Amount", "How much. Link an LFO or an envelope to it for the classic sync sweep."},
        {"osc2Warp", "Osc 2", "Osc 2's warp."},
        {"osc2WarpAmt", "Amount", "How much Osc 2 is warped."}}},
      {"play", "Play", "How the synth is played: how many notes Pad plays, the pitch bend and the mod wheel (automate them "
       "like any knob; the mod wheel is also a source of links).",
       {{"polyVoices", "Voices", "How many notes Pad plays at once (1 to 8). One more steals the oldest."},
        {"bendRange", "Bend range", "How far the pitch bend goes, in semitones, up and down."},
        {"bend", "Bend", "The pitch bend: -1 = Bend range down, +1 = up."},
        {"modWheel", "Mod wheel", "The mod wheel: moves whatever it is linked to."}}},
      {"filter2", "Filter 2", "A second filter, with the same types as the first. Send some sources to it (Routing), or "
       "turn on Serial to put it after Filter 1: a low-pass into a band, a comb after a ladder, two filters moved apart.",
       {{"filter2Mode", "Type", "What Filter 2 keeps: the same types as Filter 1 (Low 24, Band, Comb, Formant, Diode...)."},
        {"tone2", "Tone", "Where Filter 2 sits. Link an LFO or an envelope to it like Tone (in octaves)."},
        {"bite2", "Bite", "How much Filter 2 screams around its Tone."},
        {"filter2Drive", "Drive in", "Pushes the sound into Filter 2 itself: it growls as it resonates. 0 = clean."},
        {"filter2Shape", "Shape", "For Morph, Phaser, Dual Notch and Dual Band, as Filter 1's Shape."},
        {"filterSerial", "Serial", "On: Filter 1's output goes into Filter 2 (one after the other). Off: side by side."},
        {"filter2Mix", "Mix", "How much of Filter 2 you hear, the rest of what goes in dry."}}},
      {"routing", "Routing", "Where each source goes: Filter 1, Filter 2, or past the filters (Dry: a sub that stays round "
       "under a screaming filter).",
       {{"osc1Route", "Osc 1", "Where Osc 1 (with its unison and the grains) goes: Filter 1, Filter 2 or Dry."},
        {"osc2Route", "Osc 2", "Where Osc 2 goes: Filter 1, Filter 2 or Dry."},
        {"subRoute", "Sub", "Where the Sub goes: Filter 1, Filter 2 or Dry."},
        {"noiseRoute", "Noise", "Where the noise goes: Filter 1, Filter 2 or Dry."}}},
      {"shape", "Filter+", "More ways to shape the sound: which part the filter keeps, and how hard the sound is pushed.",
       {{"filterMode", "Type", "What the filter keeps. Low 24 = the lows, smooth; Low 12 = the lows, brighter; "
         "Band = only the middle around Tone (thin, nasal); High = the highs (takes the bottom away); Notch = everything but Tone; "
         "Low 6 / High 6 = gentle; Band 24 = narrow; High 12 = softer high; Ladder = the warm analog low-pass (Moog style); "
         "Comb = tuned to Tone, metallic; Formant = vowels, Tone moves A-E-I-O-U; Diode = the acid filter (TB-303): it "
         "squelches, the bass thins out as Bite rises; MS-20 Low / MS-20 High = aggressive, it screams and growls; Comb - = "
         "hollow, an octave down; Phaser = moving notches, the psychedelic sweep; Morph = low, band or high by Shape; Dual "
         "Notch / Dual Band = two filters around Tone, Shape sets how far apart."},
        {"filterDrive", "Drive in", "Pushes the sound into the filter itself: it growls and gets dirty as it resonates "
         "(as a hardware filter does). 0 = clean."},
        {"filterShape", "Shape", "For Morph: low (left), band (middle), high (right). For Phaser: how far apart its "
         "notches are. For Dual Notch / Dual Band: how far apart the two filters are (up to 2 octaves each side)."},
        {"drive", "Drive", "Pushes the sound into a soft clip after the filter: warmer, fatter, then dirty. 0 = clean."},
        {"driveColour", "Colour", "Makes the clip uneven (as a tube): richer and warmer. 0 = even."},
        {"driveMix", "Drive mix", "How much of the driven sound you hear, the rest clean: dirt under a clear sound."},
        {"crush", "Crush", "Fewer bits and a lower rate: a gritty, digital sound with aliasing (the laser stabs of "
         "psytrance). 0 = clean."}}},
      {"lfo1", "LFO 1", "A movement that repeats: in time with the song, again on each note, or once per note. It moves "
       "whatever it is linked to (a wobble, a vibrato, a tremolo, a gate, a pan).",
       {{"lfo1Rate", "Rate", "How fast, in time: beats per movement (1 = a beat, 16 = 4 bars, 0.125 = a 1/32 note)."},
        {"lfo1Hz", "Hz", "How fast when it is not in time (sync off), in movements per second."},
        {"lfo1Phase", "Phase", "Where in its movement it starts."},
        {"lfo1Smooth", "Smooth", "Glides over its jumps (square, saw, random), in ms: 0 = hard steps."},
        {"lfo1Depth", "Amount", "How far LFO 1 moves everything it is linked to. 0 = it moves nothing."}}},
      {"lfo2", "LFO 2", "A second movement, for something else at another speed: a slow filter sweep under a fast gate.",
       {{"lfo2Rate", "Rate", "How fast, in time: beats per movement."}, {"lfo2Hz", "Hz", "How fast when it is not in time."},
        {"lfo2Phase", "Phase", "Where in its movement it starts."}, {"lfo2Smooth", "Smooth", "Glides over its jumps, in ms."},
        {"lfo2Depth", "Amount", "How far LFO 2 moves everything it is linked to. 0 = it moves nothing."}}},
      {"lfo3", "LFO 3", "A third movement. By default the slow Drift of the filter and the detune, a few bars long.",
       {{"lfo3Rate", "Rate", "How fast, in time: beats per movement."}, {"lfo3Hz", "Hz", "How fast when it is not in time."},
        {"lfo3Phase", "Phase", "Where in its movement it starts."}, {"lfo3Smooth", "Smooth", "Glides over its jumps, in ms."},
        {"lfo3Depth", "Amount", "How far LFO 3 moves everything it is linked to. 0 = it moves nothing."}}},
      {"lfo4", "LFO 4", "A fourth movement.",
       {{"lfo4Rate", "Rate", "How fast, in time: beats per movement."}, {"lfo4Hz", "Hz", "How fast when it is not in time."},
        {"lfo4Phase", "Phase", "Where in its movement it starts."}, {"lfo4Smooth", "Smooth", "Glides over its jumps, in ms."},
        {"lfo4Depth", "Amount", "How far LFO 4 moves everything it is linked to. 0 = it moves nothing."}}},
      {"macros", "Macros", "Four knobs of your own: link one to several knobs and turn them all at once.",
       {{"macro1", "Macro 1", "Moves what it is linked to."}, {"macro2", "Macro 2", "Moves what it is linked to."},
        {"macro3", "Macro 3", "Moves what it is linked to."}, {"macro4", "Macro 4", "Moves what it is linked to."}}},
      {"out", "Out", "How loud it is and where it sits.", {{"level", "Volume", "How loud the whole synth is."},
                                                    {"outPan", "Output pan", "Where the synth sits, left or right."}}}};
  return layers;
}

const std::vector<WavePreset>& wavePresets() {
  // Remade on 2026-11-07 against Diogo's own synth samples (D:\Samples\Synth, 44 leads, acids, plucks and stabs of
  // Vibration, Bananas, Function Loops, GM): rendered with core-wave-ab and measured. The old presets were 16 dB
  // darker than the samples between 2.5 and 8 kHz and 46 dB darker above 8 kHz (a filter at a fixed 450-900 Hz that
  // did not follow the notes: muffled, "tiros num jogo SNES"). Now the filter follows the keys (Key follow) and rests
  // where the samples have their brightness: the leads measure -11 to -15 dB at 2.5-8 kHz (samples: -11) and a slope
  // of -6 to -11 dB per octave (samples: -6). Grouped by what they are (category), each made for a style; any of them
  // works in any style. Ids stay stable (help/wave.json and AICORE name them).
  using V = std::map<std::string, double>;
  // A knob a preset does not name gets its default (version 1's), so a preset always sets the whole sound and nothing
  // of the previous one stays.
  static const std::vector<WavePreset> presets = [] {
  std::vector<WavePreset> list = {
      {"sunrise-screamer", "Sunrise Screamer", "psytrance", "The full-on screamer: a wide supersaw with a saw an octave up, a low-pass that follows the notes and opens on each one, a little drive, the 3/16 echo.",
       V{{"wave", 3}, {"spread", 0.35}, {"tone", 2200}, {"keyTrack", 1}, {"bite", 0.3}, {"contour", 1.5}, {"bloom", 260}, {"filterSustain", 0.25}, {"osc2", 0.35}, {"osc2Wave", 3}, {"osc2Semi", 12}, {"osc2Fine", 6}, {"drive", 0.3}, {"fade", 70}, {"drift", 0.1}, {"level", 0.36}},
       1, "echo", "lead"},
      {"fifth-lead", "Fifth Lead", "psytrance", "Two saws a fifth apart, a short filter snap, a light vibrato: the bright two-oscillator lead.",
       V{{"wave", 3}, {"spread", 0.2}, {"tone", 2600}, {"keyTrack", 0.9}, {"bite", 0.3}, {"contour", 1.2}, {"bloom", 250}, {"filterSustain", 0.3}, {"osc2", 0.7}, {"osc2Wave", 3}, {"osc2Semi", 7}, {"osc2Fine", 4}, {"glide", 40}, {"fade", 80}, {"drive", 0.2}, {"lfo1Dest", 1}, {"lfo1Shape", 0}, {"lfo1Rate", 5}, {"lfo1Amount", 0.08}, {"level", 0.4}},
       1, "echo", "lead"},
      {"forest-glide", "Forest Glide", "psytrance", "One slightly doubled saw with a long slide between notes and a filter that breathes: progressive and twilight lines.",
       V{{"wave", 3}, {"spread", 0.12}, {"tone", 1800}, {"keyTrack", 1}, {"bite", 0.2}, {"contour", 1}, {"bloom", 300}, {"filterSustain", 0.3}, {"glide", 160}, {"fade", 90}, {"drift", 0.3}, {"drive", 0.15}, {"level", 0.45}},
       0.375, "echo-quarter", "lead"},
      {"goa-sync", "Goa Sync", "psytrance", "A hard-sync lead whose tone sweeps up and down over two bars: the classic Goa and full-on sync scream.",
       V{{"wave", 3}, {"spread", 0.15}, {"osc1Table", 2}, {"osc1Pos", 0.35}, {"lfo1Dest", 4}, {"lfo1Shape", 1}, {"lfo1Rate", 1}, {"lfo1Amount", 0.35}, {"tone", 4000}, {"keyTrack", 0.6}, {"bite", 0.25}, {"contour", 1}, {"bloom", 200}, {"filterSustain", 0.4}, {"drive", 0.25}, {"fade", 70}, {"level", 0.46}},
       2, "echo", "lead"},
      {"swamp-siren", "Swamp Siren", "darkpsycore", "A dark screamer with an octave under it and more scream in the filter: phrygian lines in the dark.",
       V{{"wave", 3.5}, {"spread", 0.2}, {"stack", -1}, {"tone", 1300}, {"keyTrack", 1}, {"bite", 0.55}, {"contour", 2}, {"bloom", 180}, {"filterSustain", 0.2}, {"glide", 30}, {"drive", 0.35}, {"fade", 50}, {"drift", 0.15}, {"level", 0.31}},
       0.5, "echo", "lead"},
      {"rusted-hoover", "Rusted Hoover", "darkpsycore", "The hoover: a very wide saw with a square an octave up, each note sliding up into its pitch, an octave below, grit.",
       V{{"wave", 3}, {"spread", 0.7}, {"stack", -1}, {"osc2", 0.5}, {"osc2Wave", 2}, {"osc2Semi", 12}, {"osc2Fine", -8}, {"pitchEnv", -3}, {"pitchTime", 70}, {"tone", 2400}, {"keyTrack", 0.5}, {"bite", 0.35}, {"contour", 0.8}, {"bloom", 400}, {"filterSustain", 0.5}, {"drive", 0.35}, {"fade", 120}, {"drift", 0.4}, {"level", 0.32}},
       2, "grit", "lead"},
      {"static-riser", "Static Riser", "hitech", "A hard, wide lead an octave up that drifts over four bars: FX lines and risers.",
       V{{"wave", 4}, {"spread", 0.6}, {"stack", 1}, {"tone", 5000}, {"keyTrack", 0.5}, {"bite", 0.3}, {"contour", 0.5}, {"bloom", 400}, {"filterSustain", 0.5}, {"drift", 0.8}, {"fade", 300}, {"level", 0.38}},
       4, "echo", "lead"},
      {"glass-fm-lead", "Glass FM Lead", "hitech", "A glassy FM lead (a sine with a sine at three times the note) whose metal grows and fades every bar: hi-tech melodies.",
       V{{"wave", 0}, {"fm", 2.2}, {"fmRatio", 3}, {"tone", 9000}, {"keyTrack", 0.5}, {"contour", 0}, {"osc2", 0.4}, {"osc2Wave", 3}, {"osc2Semi", 12}, {"osc2Fine", 3}, {"lfo1Dest", 6}, {"lfo1Shape", 1}, {"lfo1Rate", 3}, {"lfo1Amount", 0.3}, {"fade", 60}, {"level", 0.38}},
       1, "slap", "lead"},
      {"acid-needle", "Acid Needle", "hitech", "A 303-style acid line: one saw into the warm ladder filter that screams near the top, a short slide, a 1/8 slap.",
       V{{"wave", 3}, {"filterMode", 9}, {"tone", 280}, {"keyTrack", 0.5}, {"bite", 0.66}, {"contour", 3.5}, {"bloom", 170}, {"glide", 60}, {"drive", 0.45}, {"fade", 25}, {"level", 0.43}},
       1, "slap", "acid"},
      {"squelch-303", "Squelch 303", "darkpsycore", "A square-wave acid with a short, deep filter snap and heavy drive: the rubbery squelch of dark psy.",
       V{{"wave", 2}, {"filterMode", 9}, {"tone", 224}, {"keyTrack", 0.4}, {"bite", 0.78}, {"contour", 4}, {"bloom", 120}, {"glide", 50}, {"drive", 0.55}, {"fade", 25}, {"level", 0.37}},
       1, "grit", "acid"},
      {"night-pluck", "Night Pluck", "darkpsycore", "A short, dark pluck that dies by itself while the key is held: the off-beat stab and call-and-answer line of dark psy.",
       V{{"wave", 3}, {"spread", 0.15}, {"tone", 700}, {"keyTrack", 1}, {"bite", 0.45}, {"contour", 3}, {"bloom", 110}, {"decay", 260}, {"sustain", 0}, {"mode", 0}, {"fade", 90}, {"drive", 0.25}, {"drift", 0.1}, {"level", 0.99}},
       1, "echo", "pluck"},
      {"psy-pluck", "Psy Pluck", "psytrance", "A bright, wide pluck with an octave on top: arpeggios and melodies in the breakdown, with the 3/16 echo.",
       V{{"wave", 3}, {"spread", 0.25}, {"osc2", 0.4}, {"osc2Wave", 3}, {"osc2Semi", 12}, {"osc2Fine", 5}, {"tone", 1100}, {"keyTrack", 1}, {"bite", 0.3}, {"contour", 3.5}, {"bloom", 130}, {"decay", 320}, {"sustain", 0}, {"mode", 0}, {"fade", 120}, {"drive", 0.2}, {"level", 0.95}},
       1, "echo", "pluck"},
      {"hitech-blip", "Hi-tech Blip", "hitech", "A tiny FM blip with a click of pitch at the start: the fast, bubbly 1/16 and 1/32 answers of hi-tech.",
       V{{"wave", 0}, {"fm", 3}, {"fmRatio", 2}, {"tone", 8000}, {"keyTrack", 0.5}, {"contour", 0}, {"decay", 110}, {"sustain", 0}, {"pitchEnv", 12}, {"pitchTime", 8}, {"mode", 0}, {"fade", 40}, {"level", 1.5}},
       1, "slap", "pluck"},
      {"laser-stab", "Laser Stab", "psytrance", "A short lasery stab: two saws and a third an octave up, a fast two-octave fall in pitch, a little crush for the digital edge, a small room and the 3/16 echo.",
       V{{"wave", 3}, {"spread", 0.25}, {"tone", 7000}, {"bite", 0.25}, {"contour", 1}, {"bloom", 120}, {"decay", 220}, {"sustain", 0}, {"pitchEnv", 24}, {"pitchTime", 45}, {"osc2", 0.8}, {"osc2Wave", 3}, {"osc2Fine", 14}, {"subLevel", 0.47}, {"subOct", 1}, {"subShape", 3}, {"crush", 0.15}, {"fade", 60}, {"level", 1.36}},
       1, "laser", "stab"},
      {"laser-zap", "Laser Zap", "hitech", "A hard saw that drops two octaves in a flash on every note: lasers and zaps.",
       V{{"wave", 3.5}, {"spread", 0.1}, {"tone", 4000}, {"keyTrack", 0.3}, {"bite", 0.3}, {"contour", 1.5}, {"bloom", 80}, {"decay", 200}, {"sustain", 0}, {"pitchEnv", 24}, {"pitchTime", 40}, {"drive", 0.2}, {"fade", 30}, {"level", 0.97}},
       1, "slap", "stab"},
      {"bubble-metal", "Bubble Metal", "hitech", "Two-operator FM at an odd ratio with a fast zap: the metallic, bubbly hi-tech line.",
       V{{"wave", 0}, {"fm", 4}, {"fmRatio", 3.5}, {"tone", 8000}, {"bite", 0.2}, {"contour", 0}, {"decay", 300}, {"sustain", 0.1}, {"pitchEnv", 12}, {"pitchTime", 25}, {"fade", 40}, {"level", 0.95}},
       1, "slap", "stab"},
      {"wobble-eighth", "Wobble 1/8", "hitech", "A square and a saw with the filter swinging on every eighth note: the wobble, locked to the song.",
       V{{"wave", 2}, {"stack", -1}, {"tone", 900}, {"keyTrack", 0.5}, {"bite", 0.55}, {"osc2", 0.6}, {"osc2Wave", 3}, {"osc2Fine", 12}, {"drive", 0.4}, {"lfo1Dest", 2}, {"lfo1Shape", 1}, {"lfo1Rate", 5}, {"lfo1Amount", 0.6}, {"fade", 40}, {"level", 0.32}},
       1, "slap", "stab"},
      {"reese-tear", "Reese Tear", "darkpsycore", "Two saws an octave apart, slightly out of tune, through a low-pass that an LFO opens over two bars: the dark, tearing reese.",
       V{{"wave", 3}, {"spread", 0.2}, {"octave", -1}, {"tone", 1800}, {"keyTrack", 0.5}, {"bite", 0.35}, {"contour", 0.5}, {"bloom", 300}, {"osc2", 0.9}, {"osc2Wave", 3}, {"osc2Semi", 12}, {"osc2Fine", 9}, {"drive", 0.35}, {"glide", 40}, {"swell", 3}, {"fade", 120}, {"lfo1Dest", 2}, {"lfo1Shape", 1}, {"lfo1Rate", 1}, {"lfo1Amount", 0.35}, {"level", 0.38}},
       1, "dry", "stab"},
      {"cathedral-fog", "Cathedral Fog", "slambient", "A soft, wide supersaw pad with an octave over it that swells in and fades slowly, in a big space.",
       V{{"wave", 3}, {"spread", 0.5}, {"stack", 1}, {"tone", 3500}, {"keyTrack", 0.5}, {"bite", 0.15}, {"contour", 0.5}, {"bloom", 2000}, {"swell", 1500}, {"fade", 4000}, {"drift", 0.3}, {"mode", 1}, {"level", 0.87}},
       4, "cathedral", "pad"},
      {"dawn-strings", "Dawn Strings", "psytrance", "Wide string-like saws that open slowly on each chord: the morning breakdown pad.",
       V{{"wave", 3}, {"spread", 0.6}, {"osc2", 0.4}, {"osc2Wave", 1}, {"osc2Semi", 12}, {"tone", 4000}, {"keyTrack", 0.5}, {"bite", 0.1}, {"filterAttack", 1500}, {"filterSustain", 0.6}, {"contour", 1}, {"bloom", 3000}, {"swell", 600}, {"fade", 1500}, {"drift", 0.35}, {"mode", 1}, {"level", 0.67}},
       4, "echo-quarter", "pad"},
      {"gated-choir", "Gated Choir", "slambient", "A wide pad cut on every sixteenth by its own LFO, with the filter rising slowly: the trance-gate pad.",
       V{{"wave", 2.5}, {"spread", 0.5}, {"stack", 1}, {"tone", 3000}, {"keyTrack", 0.5}, {"bite", 0.2}, {"contour", 2}, {"bloom", 3000}, {"swell", 30}, {"fade", 400}, {"drift", 0.3}, {"mode", 1}, {"osc2", 0.5}, {"osc2Wave", 1}, {"osc2Semi", 12}, {"filterAttack", 2000}, {"filterSustain", 1}, {"lfo1Dest", 3}, {"lfo1Shape", 3}, {"lfo1Rate", 6}, {"lfo1Amount", 0.9}, {"lfo2Dest", 5}, {"lfo2Shape", 0}, {"lfo2Rate", 1}, {"lfo2Amount", 0.4}, {"level", 0.68}},
       2, "cathedral", "pad"},
      {"dark-choir", "Dark Choir", "darkpsycore", "Saws through a vowel filter that moves slowly from O to A: a dark, singing choir for intros and breaks.",
       V{{"wave", 3}, {"spread", 0.4}, {"stack", -1}, {"filterMode", 11}, {"tone", 500}, {"bite", 0.6}, {"contour", 0}, {"swell", 1200}, {"fade", 3000}, {"lfo1Dest", 2}, {"lfo1Shape", 0}, {"lfo1Rate", 0}, {"lfo1Amount", 0.4}, {"drift", 0.3}, {"mode", 1}, {"level", 1.48}},
       8, "cathedral", "pad"},
      {"abyss-drone", "Abyss Drone", "slambient", "A dark drone two octaves deep that moves over eight bars: hold one note for a whole section.",
       V{{"wave", 3}, {"spread", 0.3}, {"stack", -2}, {"tone", 600}, {"keyTrack", 0.5}, {"bite", 0.2}, {"swell", 3000}, {"fade", 6000}, {"drift", 0.5}, {"mode", 1}, {"level", 1.5}},
       8, "cathedral", "drone"},
      {"ghost-wind", "Ghost Wind", "slambient", "Noise through a slowly breathing filter, with a soft wave under it: wind, breath and the build-up hiss.",
       V{{"wave", 0}, {"spread", 0.2}, {"stack", -1}, {"tone", 3000}, {"bite", 0.35}, {"contour", 0.5}, {"bloom", 2000}, {"swell", 2000}, {"fade", 5000}, {"drift", 0.6}, {"mode", 1}, {"noise", 0.7}, {"noiseColor", 0.55}, {"level", 0.78}},
       4, "cathedral", "drone"},
      {"glass-cloud", "Glass Cloud", "slambient", "Long grains of a soft wave, scattered in pitch and pan: a shimmering cloud. Pick a sound in Grain sound to make the cloud out of it.",
       V{{"wave", 2}, {"spread", 0.3}, {"tone", 9000}, {"bite", 0.15}, {"contour", 0.3}, {"bloom", 2000}, {"swell", 1500}, {"fade", 4000}, {"drift", 0.4}, {"mode", 1}, {"grain", 0.8}, {"grainSize", 150}, {"grainDensity", 25}, {"grainPos", 0.2}, {"grainScan", 0.1}, {"grainScatter", 0.6}, {"level", 1.5}},
       4, "cathedral", "drone"},
      {"frozen-dust", "Frozen Dust", "slambient", "Short grains held on one spot of the sound: a frozen, buzzing texture. With a vocal or a drum loop in Grain sound it turns that moment into a drone.",
       V{{"wave", 2}, {"spread", 0.1}, {"tone", 4000}, {"bite", 0.2}, {"contour", 0.2}, {"bloom", 1000}, {"swell", 800}, {"fade", 2500}, {"drift", 0.2}, {"mode", 1}, {"grain", 1}, {"grainSize", 40}, {"grainDensity", 60}, {"grainPos", 0.4}, {"grainScan", 0}, {"grainScatter", 0.25}, {"level", 1.5}},
       4, "cathedral", "drone"},
  };
  // Written with version 1's knobs (Contour, Bloom, Swell, Fade, Zap, Key follow, LFO "to"), the words they were
  // measured with; turned into version 2's knobs and links here, the same conversion as a saved song's.
  const InstrumentInfo* info = findInstrumentInfo(kWaveInstrument);
  for (auto& p : list) {
    json v1 = json::object();
    for (const auto& [k, v] : p.values) v1[k] = v;
    const auto c = wlegacy::convertV1(v1);
    p.values.clear();
    for (auto it = c.params.begin(); it != c.params.end(); ++it)
      if (it.key() != "lfo3Hz") p.values[it.key()] = it.value().get<double>();
    p.synth = c.synth.is_null() ? json() : c.synth["layers"][0];
    for (const auto& x : info->params)
      if (std::string(x.name) != "lfo3Hz" && !p.values.count(x.name)) p.values[x.name] = x.def;
  }
  // The basses (2026-11-09, Diogo: "o b-core deixa de existir, vão para synth"): each one copied from one of Diogo's
  // bass one-shots by search (tools/bcore_fit/fitwave.py: layer 1 + the filter envelope env2 -> tone and the pitch
  // zap env3 -> pitch, against the sample's log-mel spectrogram over time), 0.5 to 0.9 dB from its sample where
  // B-Core's closest preset was 1.2 to 11 dB (D:\AI Dev\avaliacao-instrumentos). Of 14 close copies, the ones that
  // sounded alike were left out (5 Psiger saws -> 2, 2 Bananas deeps -> 1). Written with version 2's knobs; each is
  // -17 LUFS in a KBBB at 145 BPM (Deep Pulse -19.5 at full level: long notes).
  const std::vector<WavePreset> basses = {
      {"saw-roller-low", "Saw Roller Low", "psytrance", "A saw with its sub an octave down and a short filter pluck on each note, copied from Psiger Serum 2 Bass Saw C1.",
       V{{"bite", 0.5852}, {"drive", 0.9349}, {"env1Attack", 1.869}, {"env1Decay", 20}, {"env1Hold", 49.23}, {"env1Release", 5.189}, {"env1Sustain", 0.8994}, {"env2Attack", 0}, {"env2Decay", 188.7}, {"env2Release", 50}, {"env2Sustain", 0.1358}, {"env3Attack", 0}, {"env3Decay", 4.384}, {"env3Release", 50}, {"filterDrive", 0.028}, {"filterMode", 12}, {"filterShape", 0.9999}, {"fm", 0.5409}, {"glide", 0}, {"legato", 0}, {"level", 0.524}, {"mode", 0}, {"noise", 0.2348}, {"octave", 0}, {"osc2", 0.009435}, {"osc2Fine", -29.99}, {"osc2Semi", 0}, {"osc2Wave", 1.999}, {"subLevel", 0.2435}, {"subOct", 0}, {"subShape", 1}, {"tone", 67.32}, {"uniWidth", 0}, {"unison", 1}, {"wave", 3.405}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 3.225612823809001}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 9.476406137622785}]}")},
      {"saw-roller", "Saw Roller", "psytrance", "The rolling psytrance saw bass, brighter at the top of each 1/16, copied from Psiger Serum 2 Bass Saw C2.",
       V{{"bite", 0.4434}, {"drive", 0.4251}, {"env1Attack", 6.622}, {"env1Decay", 94.69}, {"env1Hold", 16.08}, {"env1Release", 2.912}, {"env1Sustain", 0.4081}, {"env2Attack", 0}, {"env2Decay", 135.9}, {"env2Release", 50}, {"env2Sustain", 0.1725}, {"env3Attack", 0}, {"env3Decay", 27.82}, {"env3Release", 50}, {"filterDrive", 0.7606}, {"filterMode", 1}, {"filterShape", 0.9999}, {"fm", 0.4035}, {"glide", 0}, {"legato", 0}, {"level", 0.82}, {"mode", 0}, {"noise", 0.0762}, {"octave", 0}, {"osc2", 0.1114}, {"osc2Fine", 15.91}, {"osc2Semi", 0}, {"osc2Wave", 3.133}, {"subLevel", 0.5919}, {"subOct", -1}, {"subShape", 1}, {"tone", 66.96}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.413}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 5.183712444041873}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 4.784048807591985}]}")},
      {"fullspeed-saw", "Full Speed Saw", "hitech", "A tight hi-tech saw bass, copied from Vibration Full Speed 2 BASS F1.",
       V{{"bite", 0.2627}, {"drive", 0.4818}, {"env1Attack", 16.39}, {"env1Decay", 34.43}, {"env1Hold", 128.5}, {"env1Release", 26.21}, {"env1Sustain", 0.3443}, {"env2Attack", 0}, {"env2Decay", 288.1}, {"env2Release", 50}, {"env2Sustain", 0.2122}, {"env3Attack", 0}, {"env3Decay", 203.7}, {"env3Release", 50}, {"filterDrive", 0.9391}, {"filterMode", 9}, {"filterShape", 0.9999}, {"fm", 0.475}, {"glide", 0}, {"legato", 0}, {"level", 0.556}, {"mode", 0}, {"noise", 0.1437}, {"octave", 0}, {"osc2", 0.7431}, {"osc2Fine", 20.51}, {"osc2Semi", -12}, {"osc2Wave", 0.8388}, {"subLevel", 0.5423}, {"subOct", -1}, {"subShape", 0}, {"tone", 50.72}, {"uniWidth", 0}, {"unison", 1}, {"wave", 0.6821}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 5.342174571297434}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 18.098867188437286}]}")},
      {"fullspeed-punch", "Full Speed Punch", "hitech", "A short, punchy hi-tech bass with a fast filter snap, copied from Vibration Full Speed 1 BASS F.",
       V{{"bite", 0.005827}, {"drive", 5.462e-05}, {"env1Attack", 8.728}, {"env1Decay", 3999}, {"env1Hold", 200}, {"env1Release", 372}, {"env1Sustain", 0.9999}, {"env2Attack", 0}, {"env2Decay", 190.5}, {"env2Release", 50}, {"env2Sustain", 0.04841}, {"env3Attack", 0}, {"env3Decay", 126.9}, {"env3Release", 50}, {"filterDrive", 6.611e-05}, {"filterMode", 9}, {"filterShape", 0.9999}, {"fm", 0.4944}, {"glide", 0}, {"legato", 0}, {"level", 0.704}, {"mode", 0}, {"noise", 0.2842}, {"octave", 0}, {"osc2", 0.4309}, {"osc2Fine", -5.904}, {"osc2Semi", 12}, {"osc2Wave", 2.554}, {"subLevel", 0.02803}, {"subOct", -1}, {"subShape", 1}, {"tone", 212.6}, {"uniWidth", 0}, {"unison", 1}, {"wave", 1}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 5.866051293790252}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 9.208291967731341}]}")},
      {"fullspeed-deep", "Full Speed Deep", "hitech", "A rounder, deeper hi-tech bass, copied from Vibration Full Speed 1 BASS G.",
       V{{"bite", 0.3627}, {"drive", 0.4706}, {"env1Attack", 23.11}, {"env1Decay", 70.54}, {"env1Hold", 47.87}, {"env1Release", 73.28}, {"env1Sustain", 0.7953}, {"env2Attack", 0}, {"env2Decay", 277.6}, {"env2Release", 50}, {"env2Sustain", 0.08726}, {"env3Attack", 0}, {"env3Decay", 32.38}, {"env3Release", 50}, {"filterDrive", 0.25}, {"filterMode", 9}, {"filterShape", 0.9999}, {"fm", 2.746}, {"glide", 0}, {"legato", 0}, {"level", 1.259}, {"mode", 0}, {"noise", 0.04372}, {"octave", 0}, {"osc2", 0.1091}, {"osc2Fine", -24.02}, {"osc2Semi", 7}, {"osc2Wave", 3.052}, {"subLevel", 0.7377}, {"subOct", -1}, {"subShape", 1}, {"tone", 50.47}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.721}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 2.685891639277543}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 4.897906584550814}]}")},
      {"dark-plonk", "Dark Plonk", "darkpsycore", "The dark plonk: a low note with a quick filter bite, copied from Bananas BASS.",
       V{{"bite", 0.273}, {"drive", 0.419}, {"env1Attack", 7.928}, {"env1Decay", 42.26}, {"env1Hold", 62.61}, {"env1Release", 382.2}, {"env1Sustain", 0.05975}, {"env2Attack", 0}, {"env2Decay", 513}, {"env2Release", 50}, {"env2Sustain", 0.01427}, {"env3Attack", 0}, {"env3Decay", 21.31}, {"env3Release", 50}, {"filterDrive", 0.9999}, {"filterMode", 12}, {"filterShape", 0.9999}, {"fm", 1.693}, {"glide", 0}, {"legato", 0}, {"level", 0.661}, {"mode", 0}, {"noise", 0.2884}, {"octave", 0}, {"osc2", 0.2116}, {"osc2Fine", -30}, {"osc2Semi", 0}, {"osc2Wave", 1.024}, {"subLevel", 0.981}, {"subOct", 0}, {"subShape", 1}, {"tone", 47.29}, {"uniWidth", 0}, {"unison", 1}, {"wave", 0.2996}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 6.973858946306921}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 12.019132399458487}]}")},
      {"dark-rubber", "Dark Rubber", "darkpsycore", "A rubbery dark bass, copied from Bananas BASS A.",
       V{{"bite", 0.395}, {"drive", 0.0821}, {"env1Attack", 0.3192}, {"env1Decay", 1292}, {"env1Hold", 40.34}, {"env1Release", 399.9}, {"env1Sustain", 0.1118}, {"env2Attack", 0}, {"env2Decay", 431.9}, {"env2Release", 50}, {"env2Sustain", 0.1064}, {"env3Attack", 0}, {"env3Decay", 20.67}, {"env3Release", 50}, {"filterDrive", 1}, {"filterMode", 1}, {"filterShape", 0.9999}, {"fm", 0.1401}, {"glide", 0}, {"legato", 0}, {"level", 1.004}, {"mode", 0}, {"noise", 0.006856}, {"octave", 0}, {"osc2", 0.1048}, {"osc2Fine", -30}, {"osc2Semi", 7}, {"osc2Wave", 1}, {"subLevel", 0.4003}, {"subOct", -1}, {"subShape", 1}, {"tone", 41.84}, {"uniWidth", 0}, {"unison", 1}, {"wave", 3.13}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 6.68589089680237}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 8.565978825508951}]}")},
      {"dark-bite", "Dark Bite", "darkpsycore", "A dark bass with more bite in the attack, copied from Bananas BASS F#.",
       V{{"bite", 0.2426}, {"drive", 0.008553}, {"env1Attack", 6.082}, {"env1Decay", 65.36}, {"env1Hold", 7.467}, {"env1Release", 146.3}, {"env1Sustain", 0.3994}, {"env2Attack", 0}, {"env2Decay", 294}, {"env2Release", 50}, {"env2Sustain", 0.109}, {"env3Attack", 0}, {"env3Decay", 208.5}, {"env3Release", 50}, {"filterDrive", 0.4953}, {"filterMode", 0}, {"filterShape", 0.8691}, {"fm", 1.755}, {"glide", 0}, {"legato", 0}, {"level", 1.252}, {"mode", 0}, {"noise", 0.2922}, {"octave", 0}, {"osc2", 0.2438}, {"osc2Fine", 0.2744}, {"osc2Semi", -12}, {"osc2Wave", 2.167}, {"subLevel", 0.254}, {"subOct", 0}, {"subShape", 1}, {"tone", 96.57}, {"uniWidth", 0}, {"unison", 1}, {"wave", 2.023}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 6.4463709453745714}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 1.3775602348381315}]}")},
      {"deep-pulse", "Deep Pulse", "slambient", "A soft, deep bass that swells into each note, copied from Bananas BASS DEEP. Quiet in a fast KBBB (-19.5 LUFS at full level): it is made for long notes.",
       V{{"bite", 0.7019}, {"drive", 0.1193}, {"env1Attack", 143.8}, {"env1Decay", 188.5}, {"env1Hold", 51.28}, {"env1Release", 208.3}, {"env1Sustain", 0.0917}, {"env2Attack", 0}, {"env2Decay", 23.44}, {"env2Release", 50}, {"env2Sustain", 0.1706}, {"env3Attack", 0}, {"env3Decay", 40.11}, {"env3Release", 50}, {"filterDrive", 0.1798}, {"filterMode", 1}, {"filterShape", 0.9999}, {"fm", 2.311}, {"glide", 0}, {"legato", 0}, {"level", 1.5}, {"mode", 0}, {"noise", 0.2994}, {"octave", 0}, {"osc2", 0.5645}, {"osc2Fine", -4.244}, {"osc2Semi", 0}, {"osc2Wave", 2.269}, {"subLevel", 0.002368}, {"subOct", 0}, {"subShape", 1}, {"tone", 47.91}, {"uniWidth", 0}, {"unison", 1}, {"wave", 3.05}},
       1, "dry", "bass", json::parse("{\"links\": [{\"id\": \"m1\", \"source\": \"env2\", \"target\": \"tone\", \"amount\": 4.39059911859017}, {\"id\": \"m2\", \"source\": \"env3\", \"target\": \"pitch\", \"amount\": 8.859139488062569}]}")},
  };
  for (auto p : basses) {
    for (const auto& x : info->params)
      if (std::string(x.name) != "lfo3Hz" && !p.values.count(x.name)) p.values[x.name] = x.def;
    list.push_back(std::move(p));
  }
  return list;
  }();
  return presets;
}

const std::vector<std::pair<std::string, std::string>>& waveCategories() {
  static const std::vector<std::pair<std::string, std::string>> c = {
      {"bass", "Bass"}, {"lead", "Leads"}, {"acid", "Acid"}, {"pluck", "Plucks"}, {"stab", "Stabs & FX"}, {"pad", "Pads"}, {"drone", "Drones & textures"}};
  return c;
}

const WavePreset* findWavePreset(const std::string& id) {
  for (const auto& p : wavePresets())
    if (id == p.id) return &p;
  return nullptr;
}

const std::vector<SoundMood>& waveMoods() {
  // Core's own effects after the synth (layer 0). ECHO time: 1 = 1/8, 2 = 3/16, 3 = 1/4. The low cut on leads leaves
  // the range under ~250 Hz to the bass and the kick (lead reverb short, big spaces for pads).
  static const std::vector<SoundMood> moods = {
      {"dry", "Dry", "none", "No mood: takes the mood's effects away and leaves the synth as the preset made it.", {}, {}},
      {"echo", "Echo 3/16", "psy echo", "The classic psy lead echo: repeats three sixteenths later, dark, a third of it "
       "coming back.",
       {}, {{"eq", {{"b1Freq", 250}}}, {"echo", {{"time", 2}, {"feedback", 0.33}, {"tone", 4000}, {"lowcut", 300}, {"mix", 0.25}}}}},
      {"echo-quarter", "Ping-pong 1/4", "wide echo", "Repeats on every beat, bouncing left and right.",
       {}, {{"eq", {{"b1Freq", 250}}}, {"echo", {{"time", 3}, {"feedback", 0.35}, {"pingpong", 1}, {"tone", 5000}, {"mix", 0.25}}}}},
      {"slap", "Slap 1/8", "short echo", "One or two quick repeats an eighth later: keeps an acid line tight.",
       {}, {{"eq", {{"b1Freq", 150}}}, {"echo", {{"time", 1}, {"feedback", 0.15}, {"tone", 6000}, {"mix", 0.2}}}}},
      {"cathedral", "Cathedral", "huge space", "A huge, long space for pads and drones; the very bottom stays dry.",
       {}, {{"space", {{"size", 0.9}, {"decay", 6}, {"predelay", 40}, {"tone", 6000}, {"lowcut", 300}, {"motion", 0.4}, {"mix", 0.35}}}}},
      {"laser", "Laser", "laser stab", "A small bright room and the 3/16 echo, for short laser stabs between the bass notes.",
       {}, {{"eq", {{"b1Freq", 300}}}, {"space", {{"size", 0.35}, {"decay", 1.2}, {"predelay", 10}, {"tone", 9000}, {"lowcut", 400}, {"mix", 0.22}}},
            {"echo", {{"time", 2}, {"feedback", 0.3}, {"tone", 6000}, {"lowcut", 400}, {"mix", 0.22}}}}},
      {"gated", "Gated", "rhythmic", "Chops the sound on the grid, so a pad plays in sixteenths.",
       {}, {{"eq", {{"b1Freq", 200}}}, {"pulse", {{"pattern", 1}, {"depth", 0.8}, {"smooth", 0.2}}}}},
      {"grit", "Grit", "dirty", "A little growl and dirt, then the 3/16 echo.",
       {}, {{"eq", {{"b1Freq", 250}}}, {"growl", {{"vowel", 0.3}, {"talk", 0.3}, {"grit", 10}, {"mix", 0.25}}},
            {"echo", {{"time", 2}, {"feedback", 0.3}, {"tone", 4000}, {"lowcut", 300}, {"mix", 0.2}}}}}};
  return moods;
}

const SoundMood* findWaveMood(const std::string& id) {
  for (const auto& m : waveMoods())
    if (id == m.id) return &m;
  return nullptr;
}

std::string waveMoodOf(const Track& track) {
  for (const auto& fx : track.effects)
    if (fx.is_object() && fx.contains(kWaveMoodKey) && fx[kWaveMoodKey].is_string()) return fx[kWaveMoodKey].get<std::string>();
  return "";
}

namespace {
bool isWave(const Track& t) { return t.instrument.kind == "builtin" && t.instrument.name == kWaveInstrument; }

void applyMood(Document& next, const std::string& id, const SoundMood& mood) {
  Track& t = trackById(next, id);
  if (!isWave(t)) throw DocumentError("wave", t.name + " is not played by Synth (W-Core)");
  for (int i = (int)t.effects.size() - 1; i >= 0; --i)
    if (t.effects[(size_t)i].is_object() && t.effects[(size_t)i].contains(kWaveMoodKey)) removeEffect(next, id, i);
  setMoodKnobs(next, id, kWaveMoodKey, mood.knobs);
  // The tail (Diogo 2026-11-08: Wash + Dry, the psytrance wash): a mood's space and echo are ONE Core FX, its delay
  // feeding its reverb, so the window's Wash and Dry move the whole tail.
  json tail;
  for (const auto& fx : mood.effects) {
    json values = json::object();
    for (const auto& [k, v] : fx.params) values[k] = v;
    const std::string name = fx.name;
    if (name == "space" || name == "echo") {
      const json c = coreFxFrom(name, values, next.bpm);
      if (tail.is_null()) { tail = c; continue; }
      // The second module goes into the same Core FX at the level it had alone (Amount x Return).
      const std::string m = name == "echo" ? "del" : "rev";
      for (const auto& [k, v] : c.items()) if (k.rfind(m, 0) == 0) tail[k] = v;
      const double want = c.value("amount", 0.0) * std::pow(10.0, c.value(m + "Return", 0.0) / 20.0);
      const double have = std::max(1e-6, tail.value("amount", 0.0));
      tail[m + "Return"] = std::clamp(20.0 * std::log10(std::max(1e-6, want / have)), -24.0, 24.0);
      tail["delToRev"] = 1.0;
      continue;
    }
    const int at = addEffectWith(next, id, name, values);
    trackById(next, id).effects[(size_t)at][kWaveMoodKey] = mood.id;
  }
  if (!tail.is_null()) {
    const int at = addEffectWith(next, id, "corefx", tail);
    trackById(next, id).effects[(size_t)at][kWaveMoodKey] = mood.id;
  }
}
}  // namespace

void setWaveMood(Document& doc, const std::vector<std::string>& trackIds, const std::string& moodId) {
  const SoundMood* mood = findWaveMood(moodId);
  if (!mood) throw DocumentError("wave", "no W-Core mood '" + moodId + "'");
  if (trackIds.empty()) throw DocumentError("wave", "no track chosen");
  Document next = doc;
  for (const auto& id : trackIds) applyMood(next, id, *mood);
  doc = std::move(next);
}

double waveDriftHz(double bars, double bpm) {
  bars = std::max(1.0 / 16, bars);
  return std::clamp(bpm / 60.0 / (4.0 * bars), 0.005, 8.0);
}

double waveDriftBars(const Document& doc, const Track& track) {
  const InstrumentParamInfo* p = findInstrumentParam(track, "lfo3Hz");
  const double hz = p ? instrumentParamValue(track, *p) : 0.25;
  return doc.bpm / 60.0 / (4.0 * std::max(0.005, hz));
}

void setWaveDriftBars(Document& doc, const std::string& trackId, double bars) {
  setInstrumentParam(doc, trackId, "lfo3Hz", waveDriftHz(bars, doc.bpm));
}

void switchToBuiltinInstrument(Document& doc, const std::string& trackId, const std::string& name, const char* what) {
  Track& t = trackById(doc, trackId);
  if (!playsPatterns(t)) throw DocumentError(what, t.name + " plays no notes (an audio track or a bus)");
  if (t.instrument.kind == "builtin" && t.instrument.name == name) return;
  // F-Core goes first, while the old instrument is still there to get its own values back: kept, its sampler steps
  // would write sampler knobs onto the new instrument and the engine refuses the song (sweep 2026-11-14 PL3).
  if (fcoreOf(t)) removeFCore(doc, trackId);
  dropInstrumentMoods(doc, trackId);  // the old instrument's mood effects go with it (audit 2026-11-07, 8)
  Track& tt = trackById(doc, trackId);
  tt.instrument = Instrument{};
  tt.instrument.kind = "builtin";
  tt.instrument.name = name;
  if (name == kWaveInstrument) makeFreshWave(tt.instrument);
  const InstrumentInfo* info = findInstrumentInfo(name);
  auto mine = [&](const std::string& p) {
    if (p == "gainDb" || p == "pan" || p.rfind("fx:", 0) == 0) return true;
    if (info)
      for (const auto& x : info->params) if (p == x.name) return true;
    return false;
  };
  tt.automation.erase(std::remove_if(tt.automation.begin(), tt.automation.end(), [&](const Automation& a) { return !mine(a.param); }),
                      tt.automation.end());
  json kept = json::array();
  for (const auto& m : tt.modulators) if (mine(m.value("param", ""))) kept.push_back(m);
  tt.modulators = kept;
}

std::string waveLayerParam(int layer, const std::string& param) {
  return layer <= 0 ? param : "l" + std::to_string(layer + 1) + "_" + param;
}

bool isWaveLayerParam(const std::string& param) {
  return param.size() > 3 && param[0] == 'l' && param[1] >= '2' && param[1] <= '4' && param[2] == '_';
}

bool waveLayerOn(const Track& track, int layer) {
  if (layer <= 0) return true;
  if (layer >= kWaveLayers) return false;
  const auto it = track.instrument.params.find(waveLayerParam(layer, "on"));
  return it != track.instrument.params.end() && it->second >= 0.5;
}

// The switch holds both: 0 = not added, 1 = on, kWaveLayerOff = added and switched off (the red light). The engine
// plays a layer from 0.5 up.
constexpr double kWaveLayerOff = 0.25;

bool waveLayerExists(const Track& track, int layer) {
  if (layer <= 0) return true;
  if (layer >= kWaveLayers) return false;
  const auto it = track.instrument.params.find(waveLayerParam(layer, "on"));
  return it != track.instrument.params.end() && it->second > 0.1;
}

namespace {
Track& waveTrack(Document& doc, const std::string& trackId) {
  Track& t = trackById(doc, trackId);
  if (t.instrument.kind != "builtin" || t.instrument.name != kWaveInstrument) throw DocumentError("wave", "the track is not a W-Core synth");
  return t;
}
}  // namespace

int addWaveLayer(Document& doc, const std::string& trackId) {
  const Track& t = waveTrack(doc, trackId);
  int layer = -1;
  for (int i = 1; i < kWaveLayers && layer < 0; ++i) if (!waveLayerExists(t, i)) layer = i;
  if (layer < 0) throw DocumentError("wave", "W-Core has four layers at most");
  const InstrumentInfo* info = findInstrumentInfo(kWaveInstrument);
  std::vector<std::pair<std::string, double>> copy;
  for (const auto& p : info->params) {
    const std::string name = p.name;
    if (isWaveLayerParam(name) || name == "mode" || name == "glide") continue;
    copy.push_back({waveLayerParam(layer, name), instrumentParamValue(t, p)});
  }
  const json movements = waveSynthLayer(t, 0);
  for (const auto& [k, v] : copy) setInstrumentParam(doc, trackId, k, v);
  setWaveSynthLayer(doc, trackId, layer, movements);
  setInstrumentParam(doc, trackId, waveLayerParam(layer, "on"), 1);
  return layer;
}

void setWaveLayerOn(Document& doc, const std::string& trackId, int layer, bool on) {
  waveTrack(doc, trackId);
  if (layer < 1 || layer >= kWaveLayers) throw DocumentError("wave", "no such layer");
  setInstrumentParam(doc, trackId, waveLayerParam(layer, "on"), on ? 1 : kWaveLayerOff);
}

void removeWaveLayer(Document& doc, const std::string& trackId, int layer) {
  Track& t = waveTrack(doc, trackId);
  if (layer < 1 || layer >= kWaveLayers) throw DocumentError("wave", "no such layer");
  const std::string prefix = waveLayerParam(layer, "");
  for (auto it = t.instrument.params.begin(); it != t.instrument.params.end();)
    it = it->first.rfind(prefix, 0) == 0 ? t.instrument.params.erase(it) : std::next(it);
  setWaveSynthLayer(doc, trackId, layer, json::object());
  setWaveLayerSound(doc, trackId, layer, nullptr);
}

void setWaveLayerSound(Document& doc, const std::string& trackId, int layer, const SampleRef* zone) {
  Track& t = waveTrack(doc, trackId);
  if (layer < 0 || layer >= kWaveLayers) throw DocumentError("wave", "no such layer");
  auto& zones = t.instrument.samples;
  zones.erase(std::remove_if(zones.begin(), zones.end(), [&](const SampleRef& z) { return std::clamp(z.loKey, 0, 3) == layer; }), zones.end());
  if (zone) {
    SampleRef z = *zone;
    z.rootKey = 60; z.loKey = layer; z.hiKey = 127;  // the lowest key names the layer (engine graph.cpp)
    zones.push_back(z);
    std::sort(zones.begin(), zones.end(), [](const SampleRef& a, const SampleRef& b) { return a.loKey < b.loKey; });
  }
}

const SampleRef* waveLayerSound(const Track& track, int layer) {
  for (const auto& z : track.instrument.samples) if (std::clamp(z.loKey, 0, 3) == layer) return &z;
  return nullptr;
}

bool isWaveSection(const std::string& section) {
  static const char* ids[] = {"colour", "grain", "filter", "motion", "osc2", "tables", "shape", "lfo1", "lfo2"};
  for (const char* id : ids) if (section == id) return true;
  return false;
}

std::string waveSectionParam(int layer, const std::string& section) { return waveLayerParam(layer, "sec_" + section); }

bool waveSectionOn(const Track& track, int layer, const std::string& section) {
  if (!isWaveSection(section)) return true;
  const auto it = track.instrument.params.find(waveSectionParam(layer, section));
  return it == track.instrument.params.end() || it->second >= 0.5;
}

void setWaveSectionOn(Document& doc, const std::string& trackId, int layer, const std::string& section, bool on) {
  waveTrack(doc, trackId);
  if (layer < 0 || layer >= kWaveLayers) throw DocumentError("wave", "no such layer");
  if (!isWaveSection(section)) throw DocumentError("wave", "no section '" + section + "' with a light");
  setInstrumentParam(doc, trackId, waveSectionParam(layer, section), on ? 1 : 0);
}

void setWave(Document& doc, const std::string& trackId, const std::string& presetId) {
  const WavePreset* preset = presetId.empty() ? nullptr : findWavePreset(presetId);
  if (!presetId.empty() && !preset) throw DocumentError("wave", "no W-Core preset '" + presetId + "'");
  switchToBuiltinInstrument(doc, trackId, kWaveInstrument, "wave");
  if (!preset) return;
  // A preset is the whole sound (Diogo 2026-11-08): every wave-1 knob it does not name goes back to its default, so a
  // knob turned by hand does not stay under the next preset.
  if (const InstrumentInfo* info = findInstrumentInfo(kWaveInstrument))
    for (const auto& p : info->params)
      if (!isWaveLayerParam(p.name) && !preset->values.count(p.name)) setInstrumentParam(doc, trackId, p.name, p.def);
  for (const auto& [k, v] : preset->values) setInstrumentParam(doc, trackId, k, v);
  setWaveSynthLayer(doc, trackId, 0, preset->synth.is_null() ? json::object() : preset->synth);
  // A preset is a whole sound: wave 1's cards all on again.
  for (const char* s : {"colour", "grain", "filter", "motion", "osc2", "tables", "shape", "lfo1", "lfo2"})
    setInstrumentParam(doc, trackId, waveSectionParam(0, s), 1);
  setWaveDriftBars(doc, trackId, preset->driftBars);
  if (const SoundMood* mood = findWaveMood(preset->mood)) applyMood(doc, trackId, *mood);
}

WaveSound waveSoundOf(const Document& doc, const Track& track) {
  WaveSound s;
  s.name = track.name;
  if (const InstrumentInfo* info = findInstrumentInfo(kWaveInstrument))
    for (const auto& p : info->params)
      if (std::string(p.name) != "lfo3Hz") s.values[p.name] = instrumentParamValue(track, p);
  s.synth = waveSynthLayer(track, 0);
  s.driftBars = waveDriftBars(doc, track);
  s.mood = waveMoodOf(track);
  if (s.mood.empty()) s.mood = "dry";
  if (const SampleRef* z = waveLayerSound(track, 0)) s.grain = *z;
  for (int layer = 1; layer < kWaveLayers; ++layer)
    if (const SampleRef* z = waveLayerSound(track, layer)) s.layerGrains[layer - 1] = *z;
  return s;
}

json waveSoundToJson(const WaveSound& s) {
  json values = json::object();
  for (const auto& [k, v] : s.values) values[k] = v;
  json j = {{"format", "core-wave-sound"}, {"version", 2}, {"name", s.name}, {"values", values}, {"driftBars", s.driftBars}, {"mood", s.mood}};
  if (!s.synth.is_null() && !s.synth.empty()) j["synth"] = s.synth;
  if (s.grain) j["grain"] = {{"file", s.grain->file}, {"sha256", s.grain->sha256}, {"name", s.grain->name}};
  json layers = json::array();
  for (int k = 0; k < 3; ++k)
    if (const auto& z = s.layerGrains[k])
      layers.push_back({{"layer", k + 2}, {"file", z->file}, {"sha256", z->sha256}, {"name", z->name}});
  if (!layers.empty()) { j["version"] = 2; j["layerGrains"] = layers; }
  return j;
}

WaveSound waveSoundFromJson(const json& j) {
  if (!j.is_object() || j.value("format", "") != "core-wave-sound" || !j.contains("values") || !j["values"].is_object())
    throw DocumentError("wave", "not a W-Core sound (format core-wave-sound)");
  const int version = j.value("version", 1);
  if (version > 2) throw DocumentError("wave", "this W-Core sound was saved by a newer Core");
  WaveSound s;
  s.name = j.value("name", "My sound");
  json values = j["values"], synth = j.contains("synth") ? j["synth"] : json();
  if (version < 2) {  // version 1's knobs: its movements become links (as a song's)
    const auto c = wlegacy::convertV1(values);
    values = c.params;
    synth = c.synth.is_null() ? json() : c.synth["layers"][0];
  }
  const InstrumentInfo* info = findInstrumentInfo(kWaveInstrument);
  for (const auto& [k, v] : values.items()) {
    if (!v.is_number()) continue;
    const InstrumentParamInfo* p = nullptr;
    if (info) for (const auto& x : info->params) if (k == x.name) p = &x;
    if (p && k != "lfo3Hz") s.values[k] = std::clamp(v.get<double>(), p->min, p->max);  // unknown knobs: ignored
  }
  if (!synth.is_null()) {
    try { s.synth = wmod::patchToJson(wmod::parsePatch(json{{"layers", json::array({synth})}}, wmod::targets()));
          s.synth = s.synth.is_null() ? json() : s.synth["layers"][0]; }
    catch (const std::exception& e) { throw DocumentError("wave", e.what()); }
  }
  s.driftBars = std::clamp(j.value("driftBars", 1.0), 1.0 / 16, 64.0);
  s.mood = j.value("mood", "dry");
  if (!findWaveMood(s.mood)) s.mood = "dry";
  if (j.contains("grain") && j["grain"].is_object() && j["grain"].value("file", "") != "") {
    SampleRef z;
    z.file = j["grain"].value("file", "");
    z.sha256 = j["grain"].value("sha256", "");
    z.name = j["grain"].value("name", "");
    if (z.name.empty()) z.name = toUtf8(fromUtf8(z.file).filename());
    s.grain = z;
  }
  if (j.contains("layerGrains") && j["layerGrains"].is_array())
    for (const auto& g : j["layerGrains"]) {
      if (!g.is_object() || !g.contains("layer") || !g["layer"].is_number_integer()) continue;
      const int layer = g["layer"].get<int>();
      if (layer < 2 || layer > 4 || g.value("file", "") == "") continue;
      SampleRef z;
      z.file = g.value("file", "");
      z.sha256 = g.value("sha256", "");
      z.name = g.value("name", "");
      if (z.name.empty()) z.name = toUtf8(fromUtf8(z.file).filename());
      s.layerGrains[layer - 2] = z;
    }
  return s;
}

void setWaveSound(Document& doc, const std::vector<std::string>& trackIds, const WaveSound& sound) {
  if (trackIds.empty()) throw DocumentError("wave", "no track chosen");
  Document next = doc;  // all or none
  for (const auto& id : trackIds) {
    switchToBuiltinInstrument(next, id, kWaveInstrument, "wave");
    if (const InstrumentInfo* info = findInstrumentInfo(kWaveInstrument))  // a knob the sound does not list: its default
      for (const auto& p : info->params)
        if (std::string(p.name) != "lfo3Hz") {
          const auto it = sound.values.find(p.name);
          setInstrumentParam(next, id, p.name, it != sound.values.end() ? it->second : p.def);
        }
    setWaveSynthLayer(next, id, 0, sound.synth.is_null() ? json::object() : sound.synth);
    setWaveDriftBars(next, id, sound.driftBars);
    if (const SoundMood* mood = findWaveMood(sound.mood)) applyMood(next, id, *mood);
    // The sound's own Grain sound (when its file is still there), or the wave itself.
    std::vector<SampleRef> zones;
    std::error_code ec;
    auto add = [&](const std::optional<SampleRef>& z, int layer) {
      if (!z || !fs::exists(fromUtf8(z->file), ec)) return;
      SampleRef zone = *z;
      zone.rootKey = 60; zone.loKey = layer; zone.hiKey = 127;  // the lowest key names the layer (engine graph.cpp)
      zones.push_back(zone);
    };
    add(sound.grain, 0);
    for (int k = 0; k < 3; ++k) add(sound.layerGrains[k], k + 1);
    trackById(next, id).instrument.samples = zones;
  }
  doc = std::move(next);
}

std::vector<WaveSound> loadWaveSounds(const std::filesystem::path& folder, std::vector<std::string>* problems) {
  std::vector<WaveSound> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return out;
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    try {
      std::ifstream in(f);
      WaveSound s = waveSoundFromJson(json::parse(in));
      s.file = f;
      out.push_back(std::move(s));
    } catch (const std::exception& e) {
      if (problems) problems->push_back(toUtf8(f.filename()) + ": " + e.what());
    }
  }
  std::sort(out.begin(), out.end(), [](const WaveSound& a, const WaveSound& b) { return a.name < b.name; });
  return out;
}

std::filesystem::path saveWaveSound(const std::filesystem::path& folder, const WaveSound& sound) {
  std::filesystem::create_directories(folder);
  std::string stem;
  for (char c : sound.name) stem += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '-';
  while (stem.find("--") != std::string::npos) stem.replace(stem.find("--"), 2, "-");
  if (stem.empty() || stem == "-") stem = "my-sound";
  std::filesystem::path file = folder / (stem + ".json");
  for (int k = 2; std::filesystem::exists(file); ++k) file = folder / (stem + "-" + std::to_string(k) + ".json");
  std::filesystem::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out << waveSoundToJson(sound).dump(2) << "\n";
    if (!out) throw DocumentError("wave", "could not write " + toUtf8(tmp));
  }
  std::filesystem::rename(tmp, file);
  return file;
}

// ---- the modulation (version 2)
namespace {
wmod::Patch patchOf(const Track& t) {
  try { return wmod::parsePatch(t.instrument.synth, wmod::targets()); }
  catch (const std::exception& e) { throw DocumentError("wave", e.what()); }
}
void storePatch(Document& doc, const std::string& trackId, const wmod::Patch& p) {
  // Through JSON and back: what is stored is always what the engine reads.
  json j = wmod::patchToJson(p);
  try { (void)wmod::parsePatch(j, wmod::targets()); } catch (const std::exception& e) { throw DocumentError("wave", e.what()); }
  waveTrack(doc, trackId).instrument.synth = j;
}
void checkLayer(int layer) { if (layer < 0 || layer >= kWaveLayers) throw DocumentError("wave", "no such layer"); }
wmod::Layer layerFromJson(const json& layerSynth) {
  json one = layerSynth.is_null() ? json::object() : layerSynth;
  try { return wmod::parsePatch(json{{"layers", json::array({one})}}, wmod::targets()).layer[0]; }
  catch (const std::exception& e) { throw DocumentError("wave", e.what()); }
}
json layerToJson(const wmod::Layer& l) {
  wmod::Patch p;
  p.layer[0] = l;
  const json j = wmod::patchToJson(p);
  return j.is_null() ? json::object() : j["layers"][0];
}
}  // namespace

json defaultWaveSynth() { return wlegacy::convertV1(json::object()).synth; }

void makeFreshWave(Instrument& instrument) {
  instrument.version = 2;
  instrument.synth = defaultWaveSynth();
}

json waveSynth(const Track& track) {
  const json j = wmod::patchToJson(patchOf(track));
  return j.is_null() ? json{{"layers", json::array()}} : j;
}

json waveSynthLayer(const Track& track, int layer) {
  checkLayer(layer);
  return layerToJson(patchOf(track).layer[layer]);
}

void setWaveSynth(Document& doc, const std::string& trackId, const json& synth) {
  waveTrack(doc, trackId);
  wmod::Patch p;
  try { p = wmod::parsePatch(synth, wmod::targets()); } catch (const std::exception& e) { throw DocumentError("wave", e.what()); }
  storePatch(doc, trackId, p);
}

void setWaveSynthLayer(Document& doc, const std::string& trackId, int layer, const json& layerSynth) {
  checkLayer(layer);
  wmod::Patch p = patchOf(waveTrack(doc, trackId));
  p.layer[layer] = layerFromJson(layerSynth);
  storePatch(doc, trackId, p);
}

std::string addWaveLink(Document& doc, const std::string& trackId, int layer, const std::string& source,
                        const std::string& target, double amount) {
  checkLayer(layer);
  wmod::Patch p = patchOf(waveTrack(doc, trackId));
  wmod::Layer& L = p.layer[layer];
  if ((int)L.links.size() >= wmod::kLinks) throw DocumentError("wave", "a layer has 32 links at most");
  wmod::Link k;
  int n = 1;
  auto taken = [&](const std::string& id) { for (const auto& o : L.links) if (o.id == id) return true; return false; };
  while (taken("m" + std::to_string(n))) ++n;
  k.id = "m" + std::to_string(n);
  k.source = wmod::sourceFromName(source);
  if (k.source == wmod::SrcNone) throw DocumentError("wave", "no modulation source '" + source + "'");
  k.target = target;
  k.amount = amount;
  L.links.push_back(k);
  storePatch(doc, trackId, p);  // checks the target
  return k.id;
}

void setWaveLink(Document& doc, const std::string& trackId, int layer, const std::string& linkId, const json& changes) {
  checkLayer(layer);
  if (!changes.is_object()) throw DocumentError("wave", "a link's changes are an object");
  json layerJ = waveSynthLayer(waveTrack(doc, trackId), layer);
  bool found = false;
  if (layerJ.contains("links"))
    for (auto& k : layerJ["links"])
      if (k.value("id", "") == linkId) {
        for (auto it = changes.begin(); it != changes.end(); ++it) {
          static const char* allowed[] = {"amount", "source", "target", "aux", "auxDepth", "unipolar", "on"};
          if (std::find_if(std::begin(allowed), std::end(allowed), [&](const char* a) { return it.key() == a; }) == std::end(allowed))
            throw DocumentError("wave", "a link has no '" + it.key() + "'");
          if (it.key() == "aux" && (it.value().is_null() || it.value() == "")) { k.erase("aux"); k.erase("auxDepth"); continue; }
          k[it.key()] = it.value();
        }
        found = true;
      }
  if (!found) throw DocumentError("wave", "no link '" + linkId + "'");
  setWaveSynthLayer(doc, trackId, layer, layerJ);
}

void removeWaveLink(Document& doc, const std::string& trackId, int layer, const std::string& linkId) {
  checkLayer(layer);
  wmod::Patch p = patchOf(waveTrack(doc, trackId));
  auto& links = p.layer[layer].links;
  const auto it = std::find_if(links.begin(), links.end(), [&](const wmod::Link& k) { return k.id == linkId; });
  if (it == links.end()) throw DocumentError("wave", "no link '" + linkId + "'");
  links.erase(it);
  storePatch(doc, trackId, p);
}

void setWaveLfo(Document& doc, const std::string& trackId, int layer, int lfo, const json& changes) {
  checkLayer(layer);
  if (lfo < 0 || lfo >= wmod::kLfos) throw DocumentError("wave", "no such LFO");
  if (!changes.is_object()) throw DocumentError("wave", "an LFO's changes are an object");
  json layerJ = waveSynthLayer(waveTrack(doc, trackId), layer);
  json lfos = layerJ.contains("lfos") ? layerJ["lfos"] : json::array();
  while ((int)lfos.size() <= lfo) lfos.push_back({{"shape", "sine"}, {"mode", "song"}, {"sync", true}});
  for (auto it = changes.begin(); it != changes.end(); ++it) {
    if (it.key() != "shape" && it.key() != "mode" && it.key() != "sync" && it.key() != "points")
      throw DocumentError("wave", "an LFO has no '" + it.key() + "'");
    lfos[(size_t)lfo][it.key()] = it.value();
  }
  layerJ["lfos"] = lfos;
  setWaveSynthLayer(doc, trackId, layer, layerJ);
}

void setWave(Document& doc, const std::vector<std::string>& trackIds, const std::string& presetId) {
  if (trackIds.empty()) throw DocumentError("wave", "no track chosen");
  Document next = doc;  // all or none
  for (const auto& id : trackIds) setWave(next, id, presetId);
  doc = std::move(next);
}

}  // namespace core::doc
