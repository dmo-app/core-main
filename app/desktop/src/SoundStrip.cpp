#include "SoundStrip.h"
#include "TrackColour.h"
#include <algorithm>
#include <cmath>
#include "edit.hpp"
#include "fcore.hpp"
#include "ModulationLive.h"
#include "wcore.hpp"

namespace core::app {

namespace {
juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }
using theme::kCard, theme::kText, theme::kMuted, theme::kDim, theme::kAccent;
// A column per control: its name (up to two lines) over a vertical bar, the value under it (Diogo 2026-11-07:
// "sliders na vertical, poupar espaço").
constexpr int kTitleW = 112, kKnobMax = 64, kKnobMin = 46, kGroupGap = 12, kGroupLine = 16, kLabelH = 32, kValueH = 16, kBarW = 22;
// Sections in a box (rule 11): room inside the box around the knobs, and between the boxes (Diogo 2026-11-09 "dar espaço").
constexpr int kBoxPad = 10, kBoxTop = 8, kBoxBottom = 8, kBoxGap = 14;

// What a knob changes, in plain words (Core rule: short names on show, the explanation behind the dot).
const char* tipFor(const std::string& instrument, const std::string& param) {
  struct Tip { const char *instrument, *param, *text; };
  static const Tip tips[] = {
      {"kick", "pitchStart", "How high the hit starts: higher = more click and attack."},
      {"kick", "pitchEnd", "The low note the kick lands on: its body. Tune it to the song's key."},
      {"kick", "pitchDecay", "How fast it drops from Start pitch to the Knee: short = a tight punch."},
      {"kick", "ampDecay", "How fast the kick fades after Hold. Short = tight psy kick, long = a booming tail."},
      {"kick", "hold", "How long the body stays full before it starts to fade."},
      {"kick", "click", "A short tick at the start so the kick cuts through the mix."},
      {"kick", "drive", "Distortion: rounder, louder and harder."},
      {"kick", "level", "Volume of every kick of the track. The other knobs shape the Kick row (C2); Kick Long, Punchy, Deep, Tight and Round play as copied from the library."},
      {"kick", "knee", "Where the fast first drop of the pitch stops (the punch); from there it slides slowly to End pitch."},
      {"kick", "kneeDecay", "How slowly it slides from the Knee down to End pitch: longer = a longer 'dwoon'."},
      {"kick", "length", "Where the kick stops dead: a psy kick ends before the bass comes in."},
      {"kick", "clickTone", "The pitch of the click: high = a bright tick, low = a dull knock."},
      {"hats", "tone", "Brightness: higher = thinner and brighter, lower = darker and fuller."},
      {"hats", "closedDecay", "How long the short hats ring: closed hat, pedal hat and shaker."},
      {"hats", "openDecay", "How long the long hats ring: open hat and ride (the ride rings about three times longer)."},
      {"hats", "level", "Volume of all the hat sounds."},
      {"sampler", "tune", "Plays the sample higher or lower, in semitones."},
      {"sampler", "start", "Skips the start of the sample (milliseconds)."},
      {"sampler", "attack", "How slowly each hit fades in: 0 = at once."},
      {"sampler", "decay", "Shortens the sample: how long it rings. At the right end it plays to its end."},
      {"sampler", "release", "How long it fades out after the note ends."},
      {"sampler", "pitched", "On: the keys you play change its pitch (a bass, a melody). Off: every key plays it as it is."},
      {"sampler", "level", "Volume, in dB."},
      {"sampler", "retrigger", "On: every hit restarts the sample from its start. Off: a hit that comes while the sample still plays "
                               "is skipped, so it plays once to its end (a riser on a roll). You only hear a difference when the "
                               "sample is longer than the gap between two hits."},
      {"sampler", "sustain", "How loud a held note stays after Decay. 0 = it dies away (as before); more = it holds while "
                             "the key is down (a pitched sample or a loop)."},
      {"sampler", "reverse", "On: plays the sample backwards, from its end (a reversed cymbal, a swell into a hit). Start "
                             "then counts from the end."},
      {"sampler", "filterType", "A filter on each note: Off (as before), Low (keeps the lows: darker), Band (only the "
                                "middle: thin, telephone), High (takes the bottom away)."},
      {"sampler", "cutoff", "Where the filter cuts. Low-pass: lower = darker. High-pass: higher = thinner."},
      {"sampler", "resonance", "A whistle at the cutoff: more = sharper, squelchier."},
      {"sampler", "filterEnv", "How far the filter jumps open (up) or closed (down) on each hit, in octaves, then goes "
                               "back to Cutoff. 0 = it stays still."},
      {"sampler", "filterDecay", "How fast the filter goes back to Cutoff after the hit."},
      {"sampler", "velocity", "How much louder a harder note is. Full = as hard as you play (as before), 0 = every note the same."},
  };
  for (const auto& t : tips) if (instrument == t.instrument && param == t.param) return t.text;
  return "";
}

bool isSwitch(const doc::InstrumentParamInfo& p) { return p.step == 1 && p.min == 0 && p.max == 1; }

juce::String valueText(const doc::InstrumentParamInfo& p, double v) {
  const std::string unit = p.unit, name = p.name;
  if (isSwitch(p)) return v >= 0.5 ? "On" : "Off";
  if (name == "filterType") { static const char* t[] = {"Off", "Low", "Band", "High"}; return t[std::clamp((int)std::lround(v), 0, 3)]; }
  if (unit == "oct" && p.step == 1) return v == 0 ? juce::String("0") : (v > 0 ? "+" : "") + juce::String((int)std::lround(v));
  if (unit == "Hz") return v >= 1000 ? juce::String(v / 1000.0, 1) + " kHz" : juce::String((int)std::lround(v)) + " Hz";
  if (unit == "ms") {
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, "decay") == 0 && v >= p.max - 1e-6 && p.max >= 10000) return "Full";
    return v >= 1000 ? juce::String(v / 1000.0, 2) + " s" : juce::String((int)std::lround(v)) + " ms";
  }
  if (unit == "oct") return juce::String(v, 1) + " oct";
  if (unit == "st") return (v > 0 ? "+" : "") + juce::String(v, 1) + " st";
  if (unit == "dB") return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB";
  return juce::String((int)std::lround(v / std::max(1e-9, p.max) * 100)) + " %";
}

// The Kick Designer's sections (rule 11, ui-colours.md): Click, Body, Sub, Out, in a scale of frost tones, light first.
const char* kickSection(const std::string& param) {
  if (param == "click" || param == "clickTone" || param == "pitchStart") return "CLICK";
  if (param == "pitchDecay" || param == "knee" || param == "kneeDecay" || param == "hold") return "BODY";
  if (param == "pitchEnd" || param == "ampDecay" || param == "length") return "SUB";
  return "OUT";
}
juce::Colour sectionTone(const std::string& group) {
  if (group == "CLICK") return juce::Colour(0xffb8ffff);
  if (group == "BODY") return juce::Colour(0xff7de8f2);
  if (group == "SUB") return juce::Colour(0xff4ccbe0);
  if (group == "OUT") return juce::Colour(0xff2fa3c4);
  return {};
}

// A vertical bar that offers modulation on a right-click (instead of starting a drag).
class Bar final : public juce::Slider {
public:
  Bar() : juce::Slider(juce::Slider::LinearBarVertical, juce::Slider::NoTextBox) {}
  std::function<void()> onMenu;
  void mouseDown(const juce::MouseEvent& e) override {
    menu_ = e.mods.isPopupMenu();
    if (menu_) { if (onMenu) onMenu(); return; }
    juce::Slider::mouseDown(e);
  }
  void mouseDrag(const juce::MouseEvent& e) override { if (!menu_) juce::Slider::mouseDrag(e); }
  void mouseUp(const juce::MouseEvent& e) override {
    if (menu_) { menu_ = false; return; }
    juce::Slider::mouseUp(e);
  }
private:
  bool menu_ = false;
};

const doc::Track* builtinTrack(const doc::Document& d, const std::string& id) {
  const doc::Track* t = id.empty() ? nullptr : d.findTrack(id);
  return t && t->instrument.kind == "builtin" && doc::findInstrumentInfo(t->instrument.name) ? t : nullptr;
}
}  // namespace

SoundStrip::SoundStrip(PatternEditor::Host& host) : host_(host), ab_(host) {
  title_.setText("SOUND", juce::dontSendNotification);
  title_.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, kMuted);
  name_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  name_.setColour(juce::Label::textColourId, kText);
  titleDot_.setTooltip(u8("The sound of this track's built-in instrument. Changes go to the whole track; each knob drag is one "
                          "Ctrl+Z. Double-click a knob: back to its usual value. Reset: every knob back at once."));
  openPage_.setButtonText(u8("Open"));
  openPage_.setTooltip(u8("The sound and its modulation (LFO, envelope, steps) together, on a page of their own"));
  openPage_.onClick = [this] { if (onOpenPage) onOpenPage(trackId_); };
  addChildComponent(openPage_);
  openPage_.setWantsKeyboardFocus(false);
  reset_.setButtonText(u8("Reset"));
  reset_.setTooltip(u8("Puts every knob back to its usual value: the sound this instrument starts with. One Ctrl+Z brings yours back."));
  reset_.onClick = [this] { resetSound(); };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&title_, &name_, &titleDot_, &reset_, &ab_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
}

SoundStrip::~SoundStrip() { if (dragging_) host_.finishGesture(); }

// W-Core has its own window (tabs, presets, the matrix): no card under its piano roll (Diogo 2026-11-09).
bool SoundStrip::shows() const {
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  return t && t->instrument.name != doc::kWaveInstrument;
}

void SoundStrip::setTrack(const std::string& trackId) {
  if (trackId == trackId_) return;
  if (dragging_) { host_.finishGesture(); dragging_ = false; }
  trackId_ = trackId;
  ab_.setTrack(trackId);
  documentChanged();
}

void SoundStrip::rebuild() {
  for (auto& k : knobs_) {
    removeChildComponent(k.label.get()); removeChildComponent(k.dot.get());
    if (k.slider) removeChildComponent(k.slider.get());
    if (k.value) removeChildComponent(k.value.get());
    if (k.toggle) removeChildComponent(k.toggle.get());
  }
  knobs_.clear();
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  builtFor_ = t ? t->instrument.name : std::string();
  if (!t || builtFor_ == doc::kWaveInstrument) return;
  const doc::InstrumentInfo* info = doc::findInstrumentInfo(t->instrument.name);
  // The kick's knobs in its sections' order (Click, Body, Sub, Out), each section together.
  std::vector<const doc::InstrumentParamInfo*> params;
  for (const auto& p : info->params) params.push_back(&p);
  if (t->instrument.name == "kick") {
    static const std::vector<std::string> order = {"CLICK", "BODY", "SUB", "OUT"};
    const auto rank = [](const std::string& param) {
      return std::find(order.begin(), order.end(), std::string(kickSection(param))) - order.begin();
    };
    std::stable_sort(params.begin(), params.end(), [&](const auto* a, const auto* b) { return rank(a->name) < rank(b->name); });
  }
  for (const auto* pp : params) {
    const auto& p = *pp;
    // "drums" = the kick's and the hats' knobs, prefixed: grouped under "Kick" and "Hats" with the short names.
    std::string param = p.name, group, part = t->instrument.name;
    juce::String label = u8(p.label);
    if (const auto dot = param.find('.'); dot != std::string::npos) {
      part = param.substr(0, dot);
      group = part == "kick" ? "KICK" : "HATS";
      label = label.fromFirstOccurrenceOf(" ", false, false);
    } else if (part == "kick") {
      group = kickSection(param);
    }
    Knob k;
    k.param = p.name;
    k.group = group;
    k.label = std::make_unique<juce::Label>();
    k.label->setText(label, juce::dontSendNotification);
    k.label->setFont(juce::FontOptions(theme::kFontSmall));
    k.label->setJustificationType(juce::Justification::centredBottom);
    k.label->setColour(juce::Label::textColourId, kMuted);
    k.label->setMinimumHorizontalScale(0.75f);
    k.label->setBorderSize({});
    k.dot = std::make_unique<theme::InfoDot>();
    k.dot->setTooltip(u8(tipFor(part, param.substr(param.find('.') == std::string::npos ? 0 : param.find('.') + 1))));
    if (isSwitch(p)) {
      // On/Off as a switch: a knob with two values only moved after half a turn of drag and looked broken.
      k.toggle = std::make_unique<juce::TextButton>();
      auto& b = *k.toggle;
      b.setClickingTogglesState(false);
      b.setWantsKeyboardFocus(false);
      b.setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
      b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
      b.setColour(juce::TextButton::textColourOffId, kMuted);
      const std::string name = p.name;
      juce::TextButton* bp = &b;
      b.onClick = [this, bp, name] { setParam(name, bp->getToggleState() ? 0.0 : 1.0); };
      addAndMakeVisible(*k.label);
      addAndMakeVisible(*k.dot);
      addAndMakeVisible(b);
      knobs_.push_back(std::move(k));
      continue;
    }
    // A vertical blue bar: drag up or down; the value is written under it, in its own label (a number inside a
    // 22 px bar would not fit, and the slider's own text box showed "0.0000000" for a 0 it was built with).
    auto bar = std::make_unique<Bar>();
    {
      const std::string name = p.name;
      bar->onMenu = [this, name] { showModulateMenu(name); };
    }
    k.slider = std::move(bar);
    auto& s = *k.slider;
    if (const juce::Colour tone = sectionTone(k.group); !tone.isTransparent()) theme::setIdentity(s, tone);  // the section's tone
    s.setTooltip(p.automatable ? u8("Right-click: move it with an LFO, an envelope or steps") : juce::String());
    s.setRange(p.min, p.max, p.step > 0 ? p.step : 0.0);
    if (p.logScale) s.setSkewFactorFromMidPoint(std::sqrt(std::max(1e-6, p.min) * p.max));
    s.setDoubleClickReturnValue(true, p.def);
    const doc::InstrumentParamInfo copy = p;
    s.textFromValueFunction = [copy](double v) { return valueText(copy, v); };
    s.setWantsKeyboardFocus(false);
    k.value = std::make_unique<juce::Label>();
    k.value->setFont(juce::FontOptions(theme::kFontSmall));
    k.value->setJustificationType(juce::Justification::centredTop);
    k.value->setColour(juce::Label::textColourId, kText);
    k.value->setMinimumHorizontalScale(0.7f);
    k.value->setBorderSize({});
    k.value->setInterceptsMouseClicks(false, false);
    const std::string name = p.name;
    const juce::String undoName = u8(p.label);
    juce::Slider* sp = &s;
    juce::Label* vp = k.value.get();
    s.onDragStart = [this, undoName] { host_.startGesture(undoName); dragging_ = true; };
    s.onValueChange = [this, sp, vp, name] {
      vp->setText(sp->getTextFromValue(sp->getValue()), juce::dontSendNotification);
      setParam(name, sp->getValue());
    };
    s.onDragEnd = [this] { if (dragging_) host_.finishGesture(); dragging_ = false; };
    addAndMakeVisible(*k.label);
    addAndMakeVisible(*k.dot);
    addAndMakeVisible(s);
    addAndMakeVisible(*k.value);
    showValue(k);
    knobs_.push_back(std::move(k));
  }
}

juce::Colour SoundStrip::groupTone(const std::string& group) const {
  if (group.empty()) return {};
  if (const juce::Colour tone = sectionTone(group); !tone.isTransparent()) return tone;
  std::vector<std::string> order;
  for (const auto& k : knobs_)
    if (!k.group.empty() && std::find(order.begin(), order.end(), k.group) == order.end()) order.push_back(k.group);
  if (order.size() < 2) return {};
  const int i = (int)(std::find(order.begin(), order.end(), group) - order.begin());
  return theme::toneScale(theme::identityOf(*this, theme::kBlueSoft), (int)order.size(), i);
}

void SoundStrip::documentChanged() {
  ab_.documentChanged();
  identifyWithTrack(*this, host_.currentDocument(), trackId_);  // knobs and bars in the track's colour
  for (auto& k : knobs_)  // each group's knobs in its tone (rule 11)
    if (k.slider) if (const juce::Colour tone = groupTone(k.group); !tone.isTransparent()) theme::setIdentity(*k.slider, tone);
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  const std::string instrument = t ? t->instrument.name : std::string();
  const bool ownWindow = instrument == doc::kWaveInstrument;
  if ((t ? t->instrument.name : std::string()) != builtFor_ || (t && knobs_.empty() && !ownWindow)) { rebuild(); resized(); }
  if (t) {
    const doc::InstrumentInfo* info = doc::findInstrumentInfo(instrument);
    const juce::String name = instrument == "drums" ? u8("Drum synth")
                              : instrument == "hats" ? u8("Hats synth") : instrument == "snare" ? u8("Snare + clap synth") : instrument == "kick" ? u8("Kick synth") : u8(info->label);
    name_.setText(name, juce::dontSendNotification);
    for (auto& k : knobs_)
      if (const doc::InstrumentParamInfo* p = doc::findInstrumentParam(*t, k.param)) {
        const double v = doc::instrumentParamValue(*t, *p);
        if (k.toggle) {
          k.toggle->setToggleState(v >= 0.5, juce::dontSendNotification);
          k.toggle->setButtonText(v >= 0.5 ? "On" : "Off");
        } else if (!k.slider->isMouseButtonDown()) {
          k.slider->setValue(v, juce::dontSendNotification);
          showValue(k);
        }
      }
  }
  const bool pageShows = t && !knobs_.empty() && onOpenPage != nullptr;
  if (pageShows != openPage_.isVisible()) { openPage_.setVisible(pageShows); resized(); }
  const bool resetShows = t && !knobs_.empty();
  if (resetShows != reset_.isVisible() || resetShows != ab_.isVisible()) { reset_.setVisible(resetShows); ab_.setVisible(resetShows); resized(); }
  reset_.setEnabled(t && !doc::instrumentAtDefaults(*t));
  repaint();
}

void SoundStrip::showValue(Knob& k) {
  if (k.slider && k.value) k.value->setText(k.slider->getTextFromValue(k.slider->getValue()), juce::dontSendNotification);
}

void SoundStrip::setParam(const std::string& param, double value) {
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  if (!t) return;
  const doc::InstrumentParamInfo* p = doc::findInstrumentParam(*t, param);
  if (!p) return;
  const double v = std::clamp(value, p->min, p->max);
  if (std::fabs(doc::instrumentParamValue(*t, *p) - v) < 1e-12) return;
  const std::string trackId = trackId_;
  // The sampler's filter starts fully open (Cutoff 20 kHz), which a band-pass or high-pass at that point turns into
  // silence: choosing one of those moves Cutoff to where it is heard, in the same step.
  double moveCutoff = 0;
  if (t->instrument.name == "sampler" && param == "filterType" && (std::lround(v) == 2 || std::lround(v) == 3))
    if (const auto* c = doc::findInstrumentParam(*t, "cutoff"); c && doc::instrumentParamValue(*t, *c) >= 19000)
      moveCutoff = std::lround(v) == 2 ? 1000 : 300;
  host_.applyEdit([&](doc::Document& d) {
    doc::setInstrumentParam(d, trackId, param, v);
    if (moveCutoff > 0) doc::setInstrumentParam(d, trackId, "cutoff", moveCutoff);
  }, u8(p->label));
}

void SoundStrip::resetSound() {
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  if (!t || doc::instrumentAtDefaults(*t)) return;
  if (dragging_) { host_.finishGesture(); dragging_ = false; }
  const std::string trackId = trackId_;
  host_.applyEdit([&](doc::Document& d) { doc::resetInstrumentParams(d, trackId); }, u8("Reset sound"));
}

void SoundStrip::visibilityChanged() {
  if (isVisible()) startTimerHz(30); else stopTimer();
}

void SoundStrip::timerCallback() {
  if (!isShowing()) return;
  const doc::Document& d = host_.currentDocument();
  const doc::Track* t = builtinTrack(d, trackId_);
  std::map<std::string, double> now;
  if (t && !t->modulators.empty()) {
    const double tick = host_.playheadTick();
    for (const auto& k : knobs_) {
      if (!k.slider) continue;
      bool moved = false;
      for (const auto& m : t->modulators) moved |= live::shown(m) && m.value("param", "") == k.param;
      if (!moved) continue;
      if (const doc::InstrumentParamInfo* p = doc::findInstrumentParam(*t, k.param)) now[k.param] = live::value(d, *t, *p, tick);
    }
  }
  if (now == live_) return;
  live_.swap(now);
  repaint();
}

int SoundStrip::preferredWidth() const {
  int gaps = 0;
  for (size_t i = 1; i < knobs_.size(); ++i) gaps += knobs_[i].group != knobs_[i - 1].group;
  const bool boxed = !knobs_.empty() && !groupTone(knobs_.front().group).isTransparent();
  return 20 + kTitleW + (int)knobs_.size() * kKnobMax + gaps * (boxed ? 2 * kBoxPad + kBoxGap : kGroupGap) + (boxed ? 2 * kBoxPad : 0);
}

void SoundStrip::showModulateMenu(const std::string& param) {
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  const doc::InstrumentParamInfo* p = t ? doc::findInstrumentParam(*t, param) : nullptr;
  if (!p || !p->automatable || !onModulate) return;
  juce::PopupMenu menu;
  menu.addSectionHeader(u8("Move ") + u8(p->label) + u8(" with"));
  menu.addItem(1, u8("LFO: up and down, in time with the song"));
  menu.addItem(2, u8("Envelope: a jump at every note, then back"));
  menu.addItem(3, u8("Steps: a value for every step, drawn"));
  const std::string name = param;
  menu.showMenuAsync(juce::PopupMenu::Options(), [this, name](int r) { if (r > 0 && onModulate) onModulate(name, r - 1); });
}

const SoundStrip::Knob* SoundStrip::barAt(juce::Point<int> p) const {
  for (const auto& k : knobs_) {
    if (!k.slider) continue;
    auto area = k.slider->getBounds().getUnion(k.label->getBounds());
    if (k.value) area = area.getUnion(k.value->getBounds());
    if (area.expanded(3, 0).contains(p)) return &k;
  }
  return nullptr;
}

bool SoundStrip::isInterestedInDragSource(const SourceDetails& d) {
  return onModulatorDropped != nullptr && d.description.toString().startsWith("core-modulator:");
}

void SoundStrip::itemDragMove(const SourceDetails& d) {
  const Knob* k = barAt(d.localPosition.toInt());
  const std::string on = k ? k->param : std::string();
  if (on != dropOn_) { dropOn_ = on; repaint(); }
}

void SoundStrip::itemDragExit(const SourceDetails&) {
  if (!dropOn_.empty()) { dropOn_.clear(); repaint(); }
}

void SoundStrip::itemDropped(const SourceDetails& d) {
  dropOn_.clear();
  repaint();
  const Knob* k = barAt(d.localPosition.toInt());
  if (!k || !onModulatorDropped) return;
  const std::string id = d.description.toString().fromFirstOccurrenceOf(":", false, false).toStdString();
  onModulatorDropped(id, k->param);
}

// Beside each bar a modulator moves: a thin line over the stretch it sweeps (from the bar's own value), the same
// range the Modulation panel writes under «Sweeps».
void SoundStrip::paintOverChildren(juce::Graphics& g) {
  const doc::Track* t = builtinTrack(host_.currentDocument(), trackId_);
  if (!t) return;
  for (const auto& k : knobs_) {
    if (!k.slider) continue;
    const auto b = k.slider->getBounds();
    if (k.param == dropOn_) {
      g.setColour(kAccent.withAlpha(0.9f));
      g.drawRoundedRectangle(b.toFloat().expanded(3), theme::kCorner, 1.5f);
    }
    const doc::InstrumentParamInfo* p = doc::findInstrumentParam(*t, k.param);
    if (!p) continue;
    if (const auto it = live_.find(k.param); it != live_.end()) {  // where the modulation has it now
      const float y = b.getBottom() - (float)k.slider->valueToProportionOfLength(it->second) * b.getHeight();
      g.setColour(juce::Colours::white.withAlpha(0.9f));
      g.fillRoundedRectangle((float)b.getX() - 2.0f, y - 1.0f, (float)b.getWidth() + 4.0f, 2.5f, 1.0f);
    }
    const double base = doc::instrumentParamValue(*t, *p);
    int lane = 0;
    for (const auto& m : t->modulators) {
      if (!live::shown(m) || m.value("param", "") != k.param) continue;
      const std::string shape = m.value("shape", "sine");
      const double depth = m.value("depth", 0.0);
      double wLo = -1, wHi = 1;
      if (shape == "envelope") { wLo = 0; wHi = 1; }
      if (shape == "steps" && m.contains("steps") && m["steps"].is_array() && !m["steps"].empty()) {
        wLo = 1; wHi = -1;
        for (const auto& v : m["steps"]) if (v.is_number()) { wLo = std::min(wLo, v.get<double>()); wHi = std::max(wHi, v.get<double>()); }
      }
      const bool octaves = std::string(p->unit) == "Hz";
      auto at = [&](double w) { return std::clamp(octaves ? base * std::exp2(depth * w) : base + depth * w, p->min, p->max); };
      const double y0 = k.slider->valueToProportionOfLength(at(wLo)), y1 = k.slider->valueToProportionOfLength(at(wHi));
      const float top = b.getBottom() - (float)std::max(y0, y1) * b.getHeight(), bottom = b.getBottom() - (float)std::min(y0, y1) * b.getHeight();
      const float x = (float)b.getRight() + 2.0f + 4.0f * (float)lane++;
      g.setColour(theme::kAccentBright.withAlpha(0.9f));
      g.fillRoundedRectangle(x, top, 2.5f, std::max(2.0f, bottom - top), 1.0f);
    }
  }
}

void SoundStrip::paint(juce::Graphics& g) {
  if (!shows()) return;
  g.setColour(kCard);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
  // Group names (drums: KICK, HATS) above their first knob, with a thin line over the group.
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  for (size_t i = 0; i < knobs_.size(); ++i) {
    if (knobs_[i].group.empty() || (i > 0 && knobs_[i - 1].group == knobs_[i].group)) continue;
    size_t last = i;
    while (last + 1 < knobs_.size() && knobs_[last + 1].group == knobs_[i].group) ++last;
    const int x0 = knobs_[i].label->getX(), x1 = knobs_[last].label->getRight();
    const int y = knobs_[i].label->getY() - kGroupLine;
    if (const juce::Colour tone = groupTone(knobs_[i].group); !tone.isTransparent()) {
      // A section (rule 11): its header tinted ~35% in its tone, a 2 px border in the tone around its knobs, with
      // room inside it (kBoxPad at the sides, kBoxTop under the header, kBoxBottom under the values).
      const int bottom = knobs_[last].value ? knobs_[last].value->getBottom() : knobs_[last].label->getBottom();
      const int right = std::max(x1, knobs_[last].dot->getRight());  // its last info dot inside the box
      const int top = y - kBoxTop - 2;
      const auto box = juce::Rectangle<int>(x0 - kBoxPad, top, right - x0 + 2 * kBoxPad, bottom + kBoxBottom - top).toFloat();
      g.setColour(kCard.interpolatedWith(tone, 0.35f));
      g.fillRect(box.withHeight((float)kGroupLine + 2));
      g.setColour(tone.interpolatedWith(kText, 0.25f));
      g.drawText(u8(knobs_[i].group), (int)box.getX() + 8, top + 1, 80, kGroupLine, juce::Justification::centredLeft);
      g.setColour(tone);
      g.drawRect(box, 2.0f);
      continue;
    }
    g.setColour(kDim);
    g.drawText(u8(knobs_[i].group), x0 + 4, y, 60, kGroupLine - 2, juce::Justification::centredLeft);
    g.setColour(kDim.withAlpha(0.35f));
    g.fillRect(x0 + 44, y + kGroupLine / 2 - 1, std::max(0, x1 - x0 - 48), 1);
  }
}

void SoundStrip::resized() {
  auto r = getLocalBounds().reduced(10, 8);
  auto titleArea = r.removeFromLeft(kTitleW);
  title_.setBounds(titleArea.removeFromTop(16));
  const int nameW = std::min(titleArea.getWidth() - theme::InfoDot::kSlot,
                             juce::GlyphArrangement::getStringWidthInt(name_.getFont(), name_.getText()) + 10);
  auto nameRow = titleArea.removeFromTop(20);
  name_.setBounds(nameRow.removeFromLeft(nameW));
  titleDot_.setBounds(nameRow.removeFromLeft(theme::InfoDot::kSlot).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
  if (reset_.isVisible() || openPage_.isVisible()) {
    auto buttons = titleArea.removeFromTop(30).withTrimmedTop(6);
    if (openPage_.isVisible()) { openPage_.setBounds(buttons.removeFromLeft(50)); buttons.removeFromLeft(4); }
    if (reset_.isVisible()) reset_.setBounds(buttons.removeFromLeft(54));
  }
  if (ab_.isVisible()) ab_.setBounds(r.removeFromRight(AbSwitch::kWidth).removeFromTop(24).translated(0, 2));
  if (knobs_.empty()) return;
  const bool groups = !knobs_.front().group.empty();
  const bool boxed = groups && !groupTone(knobs_.front().group).isTransparent();
  const int gap = boxed ? 2 * kBoxPad + kBoxGap : kGroupGap;
  int gaps = 0;
  for (size_t i = 1; i < knobs_.size(); ++i) gaps += knobs_[i].group != knobs_[i - 1].group;
  if (boxed) r = r.reduced(kBoxPad, 0).withTrimmedBottom(kBoxBottom - 4);
  const int w = std::clamp((r.getWidth() - gaps * gap) / (int)knobs_.size(), kKnobMin, kKnobMax);
  if (boxed) r.removeFromTop(kGroupLine + kBoxTop - 4);
  else if (groups) r.removeFromTop(kGroupLine - 6); else r.removeFromTop(2);
  int x = r.getX();
  for (size_t i = 0; i < knobs_.size(); ++i) {
    if (i > 0 && knobs_[i].group != knobs_[i - 1].group) x += gap;
    auto cell = juce::Rectangle<int>(x, r.getY(), w, r.getHeight());
    x += w;
    // The name (two short lines at most) with its dot on the right, then the bar, then the value.
    auto labelRow = cell.removeFromTop(kLabelH);
    knobs_[i].dot->setBounds(labelRow.removeFromRight(theme::InfoDot::kSlot).removeFromBottom(theme::InfoDot::kSlot + 1).withTrimmedBottom(1));
    knobs_[i].label->setBounds(labelRow.withTrimmedLeft(2));
    cell.removeFromTop(3);
    if (knobs_[i].toggle) {
      knobs_[i].toggle->setBounds(cell.withTrimmedBottom(kValueH).withSizeKeepingCentre(std::min(cell.getWidth() - 4, 46), 24));
      continue;
    }
    auto valueRow = cell.removeFromBottom(kValueH);
    knobs_[i].value->setBounds(valueRow.withTrimmedTop(1));
    knobs_[i].slider->setBounds(cell.withTrimmedBottom(2).withSizeKeepingCentre(std::min(kBarW, cell.getWidth() - 4), cell.getHeight() - 2));
  }
}

}  // namespace core::app
