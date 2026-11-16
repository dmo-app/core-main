#include "InspectorPanel.h"
#include "Language.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Theme.h"
#include "edit.hpp"

namespace core::app {

using namespace theme;

namespace {

juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }

juce::String pitchName(int p) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return juce::String(names[((p % 12) + 12) % 12]) + juce::String(p / 12 - 1);
}

// "C3", "c#2", "60": a pitch, or -1.
int parsePitch(juce::String text) {
  text = text.trim().toUpperCase();
  if (text.containsOnly("0123456789")) return text.isEmpty() ? -1 : juce::jlimit(0, 127, text.getIntValue());
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  for (int k = 11; k >= 0; --k)  // sharps first: "C#" before "C"
    if (text.startsWith(names[k])) {
      const juce::String octave = text.substring((int)std::strlen(names[k]));
      if (octave.isEmpty() || !octave.containsOnly("-0123456789")) return -1;
      return juce::jlimit(0, 127, (octave.getIntValue() + 1) * 12 + k);
    }
  return -1;
}

constexpr int64_t kLengths[] = {30, 60, 120, 240, 480, 960, 1920, 3840};
const char* kLengthNames[] = {"1/128", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
constexpr int kLengthCount = 8;
// Rolls (ratchets): menu id = count, 1 = off. Ramp: 1 flat, 2 rising (from half the velocity), 3 falling (to half).
constexpr int kRollCounts[] = {2, 3, 4, 6, 8, 12, 16};
constexpr int kHeader = 34, kRow = 34;

juce::String barBeat(int64_t tick) {
  const int64_t bar = 4 * doc::kPpq;
  return juce::String(tick / bar + 1) + "." + juce::String((tick % bar) / doc::kPpq + 1);
}

} // namespace

InspectorPanel::InspectorPanel(PatternEditor::Host& host, PatternEditor& editor) : host_(host), editor_(editor) {
  fold_.getProperties().set(kIconProperty, "left");  // a clean chevron, not the ‹ character
  fold_.setTooltip(juce::String::fromUTF8("Minimize the Inspector to a tab at the side (click the tab to open it again)"));
  fold_.onClick = [this] { setFolded(true); };
  addAndMakeVisible(fold_);
  factsScroll_.setViewedComponent(&factsView_, false);
  factsScroll_.setScrollBarsShown(true, false);
  factsScroll_.setScrollBarThickness(8);
  addChildComponent(factsScroll_);
  for (auto* s : {&pitch_, &velocity_}) {
    s->setSliderStyle(juce::Slider::IncDecButtons);
    s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 26);
    s->setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
    addChildComponent(s);
  }
  pitch_.setRange(0, 127, 1);
  pitch_.textFromValueFunction = [](double v) { return pitchName((int)v); };
  pitch_.valueFromTextFunction = [this](const juce::String& t) { const int p = parsePitch(t); return p < 0 ? pitch_.getValue() : (double)p; };
  pitch_.setTooltip("Pitch (type C3, D#2 or a MIDI number). Several notes move together.");
  pitch_.onValueChange = [this] {
    const auto notes = selectedNotes();
    if (notes.empty()) return;
    const int delta = (int)pitch_.getValue() - notes.front().pitch;
    if (delta == 0) return;
    editNotes("Pitch", [delta](doc::Note& n) { n.pitch = juce::jlimit(0, 127, n.pitch + delta); });
    editor_.revealSelection();
  };
  velocity_.setRange(1, 127, 1);
  velocity_.setTooltip("Velocity (1-127) of every selected note");
  velocity_.onValueChange = [this] {
    const double v = std::round(velocity_.getValue()) / 127.0;
    editNotes("Velocity", [v](doc::Note& n) { n.velocity = v; });
  };
  for (int i = 0; i < kLengthCount; ++i) length_.addItem(kLengthNames[i], i + 1);
  length_.setTooltip("Length of every selected note");
  length_.onChange = [this] {
    const int id = length_.getSelectedId();
    if (id < 1 || id > kLengthCount) return;
    const int64_t len = kLengths[id - 1];
    const doc::Track* t = host_.currentDocument().findTrack(editor_.track());
    const doc::Pattern* p = t ? t->findPattern(editor_.patternId()) : nullptr;
    const int64_t patternLength = p ? p->lengthTicks : len;
    editNotes("Length", [len, patternLength](doc::Note& n) { n.length = std::max<int64_t>(1, std::min(len, patternLength - n.tick)); });
  };
  addChildComponent(length_);
  roll_.addItem("Off", 1);
  for (int count : kRollCounts) roll_.addItem("x" + juce::String(count), count);
  roll_.setTooltip("Roll (ratchet): the note plays this many times in its length");
  roll_.onChange = [this] {
    const int id = roll_.getSelectedId();
    if (id < 1) return;
    editNotes("Roll", [id](doc::Note& n) {
      n.hasRoll = id > 1;
      n.roll.count = id > 1 ? id : 0;
      if (n.hasRoll && n.roll.velocityFrom == 1 && n.roll.velocityTo == 1) n.roll.velocityFrom = n.roll.velocityTo = n.velocity;  // flat at first
    });
  };
  addChildComponent(roll_);
  ramp_.addItem("Flat", 1);
  ramp_.addItem("Rise", 2);
  ramp_.addItem("Fall", 3);
  ramp_.setTooltip("How the roll's hits get louder or softer (from or to half the note's velocity)");
  ramp_.onChange = [this] {
    const int id = ramp_.getSelectedId();
    if (id < 1) return;
    editNotes("Roll ramp", [id](doc::Note& n) {
      if (!n.hasRoll) return;
      const double v = n.velocity, half = std::round(v * 50.0) / 100.0;
      n.roll.velocityFrom = id == 2 ? half : v;
      n.roll.velocityTo = id == 3 ? half : v;
    });
  };
  addChildComponent(ramp_);
  for (auto [label, text] : {std::pair<juce::Label*, const char*>{&pitchLabel_, "Pitch"}, {&lengthLabel_, "Length"}, {&velocityLabel_, "Velocity"},
                             {&rollLabel_, "Roll"}, {&rampLabel_, "Roll ramp"}}) {
    label->setText(text, juce::dontSendNotification);
    label->setColour(juce::Label::textColourId, kMuted);
    label->setFont(juce::FontOptions(theme::kFontBody));
    addChildComponent(label);
  }
  show();
}

std::vector<doc::Note> InspectorPanel::selectedNotes() const {
  std::vector<doc::Note> out;
  const doc::Track* t = host_.currentDocument().findTrack(editor_.track());
  const doc::Pattern* p = t ? t->findPattern(editor_.patternId()) : nullptr;
  if (!p) return out;
  for (const auto& n : p->notes) if (editor_.selection().count(n.id)) out.push_back(n);
  std::sort(out.begin(), out.end(), [](const doc::Note& a, const doc::Note& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
  return out;
}

void InspectorPanel::editNotes(const juce::String& label, const std::function<void(doc::Note&)>& change) {
  const std::string trackId = editor_.track(), patternId = editor_.patternId();
  const std::set<std::string> ids = editor_.selection();
  if (trackId.empty() || ids.empty()) return;
  host_.applyEdit([&](doc::Document& d) {
    for (auto& n : doc::patternById(doc::trackById(d, trackId), patternId).notes)
      if (ids.count(n.id)) change(n);
  }, label);
}

namespace {
// What a track plays, in a few words: the sampler's sample, or the built-in instrument.
juce::String soundOf(const doc::Track& t) {
  const doc::Instrument& i = t.instrument;
  if (i.kind == "audio") return "audio clips";
  if (i.name == "sampler" && !i.samples.empty())
    return "sampler: " + u8(i.samples.front().name) + (i.samples.size() > 1 ? " (" + juce::String((int)i.samples.size()) + " notes)" : juce::String());
  return u8(i.name);
}

// The filters and effects that play on a track (on, in chain order; the attachments are listed apart, where they are
// put). Diogo 2026-11-07: "o inspector deveria ter o nome do sample usado, os filtros, efeitos, tudo o que está a ser
// usado na zona selecionada".
juce::StringArray effectsOf(const doc::Track& t, juce::StringArray* off = nullptr) {
  juce::StringArray on;
  for (const auto& fx : t.effects) {
    if (!fx.is_object()) continue;
    if (fx.contains("attach")) continue;
    const std::string id = doc::effectName(fx);
    const doc::EffectInfo* info = doc::findEffect(id);
    const juce::String name = fx.value("type", std::string()) == "plugin" && fx.contains("plugin")
                                  ? u8(fx["plugin"].value("name", std::string("Plugin")))
                                  : info ? u8(info->label) : u8(id);
    if (doc::effectBypassed(fx)) { if (off) off->add(name); }
    else on.add(name);
  }
  return on;
}

void fact(std::vector<InspectorLine>& lines, const juce::String& label, const juce::String& value, bool changed = false) {
  lines.push_back({InspectorLine::Kind::Fact, label, value, changed});
}
void note(std::vector<InspectorLine>& lines, const juce::String& text) {
  if (text.isNotEmpty()) lines.push_back({InspectorLine::Kind::Note, {}, text, false});
}
void heading(std::vector<InspectorLine>& lines, const juce::String& text) { lines.push_back({InspectorLine::Kind::Heading, text, {}, false}); }

// One fact per line, the first with its label (the panel shows one line each).
void addList(std::vector<InspectorLine>& facts, const juce::String& label, const juce::StringArray& items) {
  for (int i = 0; i < items.size(); ++i) fact(facts, i == 0 ? label : juce::String(), items[i]);
}

// A value as the knob shows it: a choice by its name, kHz above 1000 Hz, 0..1 knobs in %.
juce::String valueText(double v, const char* unit, double max, double step, const std::vector<const char*>* choices = nullptr) {
  const juce::String u = unit ? juce::String(unit) : juce::String();
  if (choices && step >= 1 && !choices->empty()) {
    const int k = juce::jlimit(0, (int)choices->size() - 1, (int)std::lround(v));
    return u8((*choices)[(size_t)k]);
  }
  auto num = [](double x, int decimals) { return juce::String(x, decimals).trimCharactersAtEnd("0").trimCharactersAtEnd("."); };
  if (u == "Hz" && std::abs(v) >= 1000) return num(v / 1000.0, 2) + " kHz";
  if (u == "ms" && std::abs(v) >= 1000) return num(v / 1000.0, 2) + " s";
  if (u.isEmpty() && step == 0 && max <= 1.5) return juce::String(std::lround(v * 100)) + " %";
  const juce::String n = num(v, std::abs(v) >= 100 ? 0 : std::abs(v) >= 10 ? 1 : 2);
  return u.isEmpty() ? n : n + " " + u;
}

juce::String soundDescription(const doc::Track& t) {
  const doc::Instrument& i = t.instrument;
  if (i.kind == "audio") return "Plays audio files as they are, placed on the timeline.";
  if (i.kind == "bus") return "A bus: other tracks send their sound here, so one set of effects plays on all of them.";
  if (i.kind == "plugin") return "Played by a plugin: double-click the track's name to open its window.";
  if (i.name == "kick") return "Core's own kick: a sine that falls fast from Start pitch to End pitch. The fall is the punch, the low end is the body, "
                               "Click adds the attack and Decay sets how long it sounds.";
  if (i.name == "hats") return "Core's own hats: bright filtered noise. Closed hats are short, open hats ring longer; Tone sets how bright.";
  if (i.name == "snare") return "Core's own snare and clap: a short tone with noise for the snare, quick bursts of noise for the clap.";
  if (i.name == "drums") return "Kick and hats on one track: kick on C1 (36), closed hats on F#1 (42), open hats on A#1 (46).";
  if (i.name == "sampler") return "Plays your samples: each zone is one file over a range of keys, tuned from its root key.";
  return {};
}

juce::String effectDescription(const std::string& id) {
  if (id == "eq") return "EQ: turns the lows, mids and highs up or down.";
  if (id == "filter") return "Filter: cuts the highs (low-pass) or the lows (high-pass) above or below the cutoff.";
  if (id == "compressor") return "Compressor: brings loud parts down so the sound is more even and sits forward.";
  if (id == "saturation") return "Saturation: drives the sound into soft distortion: warmer, louder, more harmonics.";
  if (id == "delay") return "Delay: repeats the sound after a time, each repeat softer.";
  if (id == "reverb") return "Reverb: the sound in a room; size and decay say how big.";
  if (id == "limiter") return "Limiter: never lets the sound go over its ceiling.";
  if (id == "utility") return "Utility: volume, width and pan, without colouring the sound.";
  return {};
}

// The sound's settings and the effects' settings, with what each thing is: the part of the Inspector to learn from.
void describeSound(std::vector<InspectorLine>& lines, const doc::Track& t) {
  note(lines, soundDescription(t));
  if (t.instrument.kind != "builtin") return;
  const doc::InstrumentInfo* info = doc::findInstrumentInfo(t.instrument.name);
  if (!info || info->params.empty()) return;
  // Short catalogues (the kick, the hats): every knob, the changed ones in the accent. Long ones (W-Core):
  // only what moved from the default, else the list would hide everything under it.
  const bool all = info->params.size() <= 10;
  int same = 0;
  heading(lines, "SOUND SETTINGS");
  for (const auto& p : info->params) {
    const double v = doc::instrumentParamValue(t, p);
    const bool changed = std::abs(v - p.def) > 1e-9;
    if (!all && !changed) { ++same; continue; }
    fact(lines, u8(p.label), valueText(v, p.unit, p.max, p.step), changed);
  }
  if (same > 0) note(lines, juce::String(same) + " more at their default (the knobs in the instrument's window).");
}

void describeEffects(std::vector<InspectorLine>& lines, const doc::Track& t) {
  heading(lines, "EFFECTS");
  bool any = false;
  for (const auto& fx : t.effects) {
    if (!fx.is_object() || fx.contains("attach")) continue;
    any = true;
    const bool off = doc::effectBypassed(fx);
    const std::string id = doc::effectName(fx);
    const doc::EffectInfo* info = doc::findEffect(id);
    fact(lines, off ? "Off" : "On", info ? u8(info->label) : u8(id));
    note(lines, effectDescription(id));
    if (!info) continue;
    int same = 0;
    for (const auto& p : info->params) {
      const double v = doc::effectParam(fx, p.name, p.def);
      if (std::abs(v - p.def) <= 1e-9) { ++same; continue; }
      fact(lines, "   " + u8(p.label), valueText(v, p.unit, p.max, p.step, &p.choices), true);
    }
    if (same == (int)info->params.size()) note(lines, "Every knob at its default.");
  }
  if (!any) note(lines, "No effects: the sound goes to the mixer as the instrument makes it.");
}
}  // namespace

void InspectorPanel::update(const std::string& selectedTrack, const std::string& selectedClip) {
  const auto notes = selectedNotes();
  const doc::Document& d = host_.currentDocument();
  // What is shown, as text: redrawn only when it changes.
  juce::String sig = u8(selectedTrack) + "|" + u8(selectedClip) + "|" + u8(editor_.track()) + "|" + u8(editor_.patternId());
  for (const auto& n : notes) sig << "|" << u8(n.id) << ":" << n.pitch << ":" << (int)n.length << ":" << (int)std::round(n.velocity * 127) << ":" << (n.hasRoll ? n.roll.count : 0) << ":" << (int)std::round(n.roll.velocityFrom * 100) << ":" << (int)std::round(n.roll.velocityTo * 100);
  if (const doc::Track* t = d.findTrack(selectedTrack)) {
    sig << "|" << u8(t->name) << ":" << t->mix.gainDb << ":" << t->mix.pan << ":" << (int)t->mix.mute << (int)t->mix.solo;
    sig << "|" << soundOf(*t) << ":" << t->instrument.version << ":" << (int)t->effects.size();
    juce::StringArray off;
    sig << "|" << effectsOf(*t, &off).joinIntoString(",") << "/" << off.joinIntoString(",");  // a new sound (a sample, a preset) shows at once
    for (const auto& z : t->instrument.samples) sig << "|" << u8(z.name) << ":" << z.rootKey << ":" << z.loKey << ":" << z.hiKey;
    for (const auto& [k, v] : t->instrument.params) sig << "|" << u8(k) << "=" << v;  // a knob turned shows at once
    for (const auto& fx : t->effects) sig << "|" << u8(fx.dump());
    for (const auto& c : t->clips)
      if (c.id == selectedClip) {
        sig << "|" << (int)c.startTick << ":" << (int)c.lengthTicks << ":" << u8(c.patternId);
      }
  }
  if (sig.toStdString() == seen_) return;
  seen_ = sig.toStdString();
  track_ = selectedTrack;
  clip_ = selectedClip;
  facts_.clear();
  const doc::Track* t = d.findTrack(selectedTrack);
  mode_ = !notes.empty() ? Mode::Notes : t && !selectedClip.empty() ? Mode::Clip : t ? Mode::Track : Mode::Nothing;
  if (mode_ == Mode::Notes) {
    const bool samePitch = std::all_of(notes.begin(), notes.end(), [&](const doc::Note& n) { return n.pitch == notes.front().pitch; });
    const bool sameVelocity = std::all_of(notes.begin(), notes.end(), [&](const doc::Note& n) { return std::lround(n.velocity * 127) == std::lround(notes.front().velocity * 127); });
    const bool sameLength = std::all_of(notes.begin(), notes.end(), [&](const doc::Note& n) { return n.length == notes.front().length; });
    if (!pitch_.isMouseButtonDown(true)) pitch_.setValue(notes.front().pitch, juce::dontSendNotification);
    if (!velocity_.isMouseButtonDown(true)) velocity_.setValue(std::max(1.0, std::round(notes.front().velocity * 127)), juce::dontSendNotification);
    pitch_.setTooltip(samePitch ? "Pitch (type C3, D#2 or a MIDI number)" : "Pitch of the first note; a change moves every selected note by as much");
    velocity_.setTooltip(sameVelocity ? "Velocity (1-127)" : "The notes have different velocities: a value sets them all");
    int lengthId = 0;
    for (int i = 0; i < kLengthCount; ++i) if (sameLength && notes.front().length == kLengths[i]) lengthId = i + 1;
    length_.setSelectedId(lengthId, juce::dontSendNotification);
    if (lengthId == 0) length_.setText(sameLength ? juce::String(notes.front().length) + " ticks" : juce::String("mixed"), juce::dontSendNotification);
    const auto rollOf = [](const doc::Note& n) { return n.hasRoll && n.roll.count > 1 ? n.roll.count : 1; };
    const auto rampOf = [](const doc::Note& n) {
      return !n.hasRoll ? 1 : n.roll.velocityFrom < n.roll.velocityTo - 0.005 ? 2 : n.roll.velocityFrom > n.roll.velocityTo + 0.005 ? 3 : 1;
    };
    const bool sameRoll = std::all_of(notes.begin(), notes.end(), [&](const doc::Note& n) { return rollOf(n) == rollOf(notes.front()); });
    const bool sameRamp = std::all_of(notes.begin(), notes.end(), [&](const doc::Note& n) { return rampOf(n) == rampOf(notes.front()); });
    roll_.setSelectedId(sameRoll && roll_.indexOfItemId(rollOf(notes.front())) >= 0 ? rollOf(notes.front()) : 0, juce::dontSendNotification);
    if (roll_.getSelectedId() == 0) roll_.setText(sameRoll ? "x" + juce::String(rollOf(notes.front())) : juce::String("mixed"), juce::dontSendNotification);
    ramp_.setSelectedId(sameRamp ? rampOf(notes.front()) : 0, juce::dontSendNotification);
    if (!sameRamp) ramp_.setText("mixed", juce::dontSendNotification);
    ramp_.setEnabled(std::any_of(notes.begin(), notes.end(), [](const doc::Note& n) { return n.hasRoll; }));
  } else if (mode_ == Mode::Clip) {
    for (const auto& c : t->clips)
      if (c.id == selectedClip) {
        const doc::Pattern* p = t->findPattern(c.patternId);
        fact(facts_, "Track", u8(t->name));
        // What the clip sounds with: Samples > Use the best samples / Shuffle change it, and it shows here at once
        // (Diogo 2026-11-07: with a clip selected, the swap was not visible anywhere).
        fact(facts_, "Sound", soundOf(*t));
        note(facts_, soundDescription(*t));
        fact(facts_, "Pattern", (p ? u8(p->name) : juce::String("?")));
        fact(facts_, "Start", "bar " + barBeat(c.startTick));
        fact(facts_, "Length", juce::String(c.lengthTicks / (4.0 * doc::kPpq), 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + " bars");
        describeEffects(facts_, *t);  // everything that plays on it: the track's plugin effects
      }
    for (const auto& c : t->audioClips)
      if (c.id == selectedClip) {
        const doc::Asset* a = d.findAsset(c.assetId);
        fact(facts_, "Track", u8(t->name));
        fact(facts_, "File", (a ? u8(a->name) : juce::String("?")));
        fact(facts_, "Start", "bar " + barBeat(c.startTick));
        fact(facts_, "Gain", juce::String(c.gainDb, 1) + " dB");
      }
  } else if (mode_ == Mode::Track) {
    fact(facts_, "Name", u8(t->name));
    fact(facts_, "Sound", soundOf(*t));
    // The sampler's zones: which file plays which keys (the first zone holding a key plays it).
    const auto& zones = t->instrument.samples;
    if (t->instrument.kind == "builtin" && !zones.empty()) {
      const size_t shown = zones.size() <= 4 ? zones.size() : 3;
      for (size_t i = 0; i < shown; ++i) {
        const auto& z = zones[i];
        const juce::String keys = z.loKey == 0 && z.hiKey == 127 ? u8("all keys") : z.loKey == z.hiKey ? pitchName(z.loKey) : pitchName(z.loKey) + u8(" to ") + pitchName(z.hiKey);
        fact(facts_, i == 0 ? juce::String("Zones") : juce::String(), u8(z.name) + "  " + keys);
      }
      if (shown < zones.size()) fact(facts_, {}, "+" + juce::String((int)(zones.size() - shown)) + " more zones");
    }
    fact(facts_, "Volume", juce::String(t->mix.gainDb, 1) + " dB");
    fact(facts_, "Pan", (std::abs(t->mix.pan) < 0.005 ? juce::String("C") : juce::String(std::abs(t->mix.pan) * 100, 0) + (t->mix.pan < 0 ? " L" : " R")));
    if (t->mix.mute || t->mix.solo) fact(facts_, "State", juce::String(t->mix.mute ? "muted" : "") + (t->mix.mute && t->mix.solo ? ", " : "") + (t->mix.solo ? "solo" : ""));
    describeSound(facts_, *t);
    describeEffects(facts_, *t);
  }
  show();
}

// ---- the facts, one under the other

namespace {
constexpr int kFactRow = 24, kHeadingRow = 28, kLabelWidth = 92;
const juce::FontOptions kNoteFont(theme::kFontSmall);
}

int InspectorFacts::heightOf(const InspectorLine& line, int width) const {
  if (line.kind == InspectorLine::Kind::Fact) return kFactRow;
  if (line.kind == InspectorLine::Kind::Heading) return kHeadingRow;
  juce::AttributedString text;
  text.append(lang::tr(line.value), juce::Font(kNoteFont), kMuted);
  text.setWordWrap(juce::AttributedString::byWord);
  juce::TextLayout layout;
  layout.createLayout(text, (float)std::max(40, width));
  return (int)std::ceil(layout.getHeight()) + 6;
}

void InspectorFacts::setLines(std::vector<InspectorLine> lines, int width) {
  lines_ = std::move(lines);
  int h = 0;
  for (const auto& line : lines_) h += heightOf(line, width);
  setSize(std::max(40, width), h + 4);
  repaint();
}

void InspectorFacts::paint(juce::Graphics& g) {
  const int w = getWidth();
  int y = 0;
  for (const auto& line : lines_) {
    const int h = heightOf(line, w);
    const juce::Rectangle<int> row(0, y, w, h);
    y += h;
    if (line.kind == InspectorLine::Kind::Heading) {
      g.setColour(kOutline);
      g.fillRect(0, row.getY() + 6, w, 1);
      g.setColour(kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold).withKerningFactor(0.06f));
      g.drawText(lang::tr(line.label), row.withTrimmedTop(8), juce::Justification::centredLeft);
    } else if (line.kind == InspectorLine::Kind::Note) {
      juce::AttributedString text;
      text.append(lang::tr(line.value), juce::Font(kNoteFont), kMuted);
      text.setWordWrap(juce::AttributedString::byWord);
      text.draw(g, row.withTrimmedBottom(6).toFloat());
    } else {
      auto r = row;
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.setColour(kMuted);
      g.drawText(lang::tr(line.label), r.removeFromLeft(kLabelWidth), juce::Justification::centredLeft, true);
      g.setColour(line.changed ? kAccentBright : kText);
      g.drawText(line.value, r, juce::Justification::centredRight, true);
    }
  }
}

void InspectorPanel::setFolded(bool folded) {
  if (folded == folded_) return;
  folded_ = folded;
  if (onFold) onFold(folded_);
  show();
}

void InspectorPanel::mouseUp(const juce::MouseEvent& e) {
  if (folded_ && e.mouseWasClicked()) setFolded(false);
}

void InspectorPanel::show() {
  if (folded_) {  // only the tab
    for (auto* c : getChildren()) c->setVisible(false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    repaint();
    return;
  }
  setMouseCursor(juce::MouseCursor::NormalCursor);
  fold_.setVisible(true);
  const bool notes = mode_ == Mode::Notes;
  for (juce::Component* c : std::initializer_list<juce::Component*>{&pitch_, &velocity_, &length_, &roll_, &ramp_, &pitchLabel_, &lengthLabel_, &velocityLabel_, &rollLabel_, &rampLabel_})
    c->setVisible(notes);
  factsScroll_.setVisible(mode_ == Mode::Clip || mode_ == Mode::Track);
  resized();
  repaint();
}

void InspectorPanel::resized() {
  if (folded_) return;
  fold_.setBounds(getLocalBounds().reduced(8, 8).removeFromTop(kHeader - 8).removeFromRight(26).reduced(0, 2));
  auto r = getLocalBounds().reduced(12, 8);
  r.removeFromTop(kHeader + 24);
  for (auto [label, control] : {std::pair<juce::Label*, juce::Component*>{&pitchLabel_, &pitch_}, {&lengthLabel_, &length_}, {&velocityLabel_, &velocity_},
                                {&rollLabel_, &roll_}, {&rampLabel_, &ramp_}}) {
    auto row = r.removeFromTop(kRow);
    label->setBounds(row.removeFromLeft(70));
    control->setBounds(row.removeFromRight(std::min(130, row.getWidth())).reduced(0, 3));
  }
  // The facts: under the section's name (as paint() lays it out), down to the bottom; they scroll when longer.
  // Quantize and the preview of the selected notes act on notes, not on the track: they are in the piano roll's
  // Edit > Timing menu now (Diogo 2026-11-09).
  factsTop_ = 8 + (kHeader - 8) + 8 + 20;
  factsScroll_.setBounds(12, factsTop_, getWidth() - 24 + 6, std::max(0, getHeight() - 12 - factsTop_));
  factsView_.setLines(facts_, factsScroll_.getWidth() - 8 - 4);
}

void InspectorPanel::paint(juce::Graphics& g) {
  g.setColour(kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
  if (folded_) {  // the name down the tab, as a folded effect card
    const auto r = getLocalBounds().toFloat();
    g.setColour(kOutline);
    g.drawRoundedRectangle(r.reduced(0.5f), theme::kCorner, 1.0f);
    g.setColour(kMuted);
    drawIcon(g, "inspector", juce::Rectangle<float>(r.getX() + 5, 8, r.getWidth() - 10, 20));
    g.saveState();
    g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi).translated(r.getWidth(), 0));
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
    const juce::String what = mode_ == Mode::Notes ? juce::String("NOTES") : mode_ == Mode::Clip ? juce::String("CLIP") : mode_ == Mode::Track ? juce::String("TRACK") : juce::String();
    g.drawText(lang::tr("INSPECTOR") + (what.isNotEmpty() ? "  ·  " + lang::tr(what) : juce::String()), juce::Rectangle<float>(36, 0, r.getHeight() - 44, r.getWidth()),
               juce::Justification::centredLeft, true);
    g.restoreState();
    return;
  }
  auto r = getLocalBounds().reduced(12, 8);
  auto head = r.removeFromTop(kHeader - 8);
  g.setColour(kMuted);
  drawIcon(g, "inspector", head.removeFromLeft(20).toFloat());
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
  g.drawText(lang::tr("INSPECTOR"), head.withTrimmedLeft(8), juce::Justification::centredLeft);
  r.removeFromTop(8);
  g.setColour(kOutline);
  g.fillRect(r.getX(), r.getY() - 4, r.getWidth(), 1);
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold).withKerningFactor(0.06f));
  const int count = (int)editor_.selection().size();
  const juce::String section = mode_ == Mode::Notes ? (count == 1 ? juce::String("NOTE (1 SELECTED)") : "NOTES (" + juce::String(count) + " SELECTED)")
                               : mode_ == Mode::Clip ? juce::String("CLIP")
                               : mode_ == Mode::Track ? juce::String("TRACK")
                                                      : juce::String("NOTHING SELECTED");
  g.drawText(section, r.removeFromTop(20), juce::Justification::centredLeft);
  if (mode_ == Mode::Nothing) {
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawFittedText(lang::tr("Select a track, a clip, or notes in the piano roll."), r.removeFromTop(40), juce::Justification::topLeft, 3);
  }
}

} // namespace core::app
