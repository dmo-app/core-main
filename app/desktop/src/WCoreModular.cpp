#include "WCoreModular.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "AbSwitch.h"
#include "core_plugins.hpp"
#include "Theme.h"
#include "TrackColour.h"
#include "WCoreText.h"
#include "edit.hpp"
#include "sound_layers.hpp"
#include "../../../engine/src/dsp/wave_mod.hpp"

namespace core::app {

namespace {
juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }

// The Core Studio look (Diogo 2026-11-09, "normalizar o ui todo"): the theme's surfaces, square corners, and the
// track's own colour for knobs, curves and lights (the window is the track's: theme::identityOf), not a cyan of its own.
const juce::Colour kBg = theme::kBackground, kPanelC = theme::kCard, kInner = theme::kField, kLine = theme::kOutline,
    kTextC = theme::kText, kMutedC = theme::kMuted, kDimC = theme::kDim;
constexpr int kHeaderH = 120, kCardHead = 30, kScreenH = 56, kKnobW = 70, kKnobH = 74, kLinkH = 30, kPad = 10, kSelectH = 24,
              kOptionH = 28, kMaxCols = 5, kZoneCol = 24, kZoneBar = 26, kKnobMin = 50, kBodyPad = 8, kFileH = 22;
// The rows of the OSC tab (the design Diogo approved, 2026-11-09 "esta ja feito"): the name of each row is vertical
// in the narrow column on the left, and a click there minimises the row to a bar.
const char* const kZoneNames[6] = {"SOURCES", "TIMBRE", "ENV", "LFO", "FX", "GLOBAL"};
constexpr int kZones = 6;
// FX: the modules of the sound's tail (the mood's Core FX on the track), shown when the mood has one.
bool isTailModule(const std::string& id) { return id == "delay" || id == "reverb"; }
// A tail parameter's value in words: its choice (1/8, 1/4 D...), dB, seconds, ms or percent.
juce::String tailText(const doc::EffectParamInfo& p, double v) {
  const int i = (int)std::lround(v);
  if (p.step >= 1 && !p.choices.empty()) return juce::String::fromUTF8(p.choices[(size_t)std::clamp(i, 0, (int)p.choices.size() - 1)]);
  const std::string unit = p.unit ? p.unit : "";
  if (unit == "dB") return (v > 0.05 ? "+" : "") + juce::String(v, 1) + " dB";
  if (unit == "s") return juce::String(v, v < 10 ? 1 : 0) + " s";
  if (unit == "ms") return juce::String((int)std::lround(v)) + " ms";
  return juce::String(juce::roundToInt(v * 100)) + " %";
}
const doc::EffectParamInfo* tailParam(const std::string& name) {
  if (const doc::EffectInfo* e = doc::findEffect("corefx"))
    for (const auto& p : e->params) if (name == p.name) return &p;
  return nullptr;
}

// A parameter that is a choice between named values: a list, not a knob (Diogo 2026-11-09 "se for switch usar switch").
bool isChoice(const std::string& name) {
  for (const char* n : {"osc1Table", "osc2Table", "osc1Route", "osc2Route", "subRoute", "noiseRoute", "filterMode", "filter2Mode",
                        "subShape", "osc1Warp", "osc2Warp"})
    if (name == n) return true;
  return false;
}
// On / off: a switch.
bool isSwitch(const std::string& name) { return name == "legato" || name == "glideAlways" || name == "filterSerial"; }
// The list a module's type label opens, in its header: the oscillator's table, the sub's shape, the filter's mode.
const char* typeParam(const std::string& id) {
  if (id == "osc1") return "osc1Table";
  if (id == "osc2") return "osc2Table";
  if (id == "sub") return "subShape";
  if (id == "filter1") return "filterMode";
  if (id == "filter2") return "filter2Mode";
  return nullptr;
}

juce::Font font(float size, bool bold = false) { return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)); }

// The words for a modulation source.
juce::String sourceLabel(const std::string& s) {
  if (s.rfind("lfo", 0) == 0) return "LFO " + juce::String(s.substr(3));
  if (s.rfind("env", 0) == 0) return "ENV " + juce::String(s.substr(3));
  if (s.rfind("macro", 0) == 0) return "Macro " + juce::String(s.substr(5));
  if (s == "velocity") return u8("Velocidade");
  if (s == "note") return u8("Nota");
  if (s == "modwheel") return u8("Mod wheel");
  return u8(s);
}

// Where a link can go: the knobs a link can move (with their names) and the four virtual destinations.
struct TargetChoice { std::string id; juce::String label; double range; };
const std::vector<TargetChoice>& targetChoices() {
  static const std::vector<TargetChoice> list = [] {
    std::vector<TargetChoice> out = {{"pitch", u8("Afina\xc3\xa7\xc3\xa3o (st)"), 48}, {"volume", u8("Volume"), 1},
                                     {"pan", u8("Pan"), 1}, {"detune", u8("Detune (unison)"), 2}};
    const doc::InstrumentInfo* info = doc::findInstrumentInfo(doc::kWaveInstrument);
    for (const auto& t : wmod::targets()) {
      juce::String label = u8(t);
      double range = 1;
      if (info)
        for (const auto& p : info->params)
          if (t == p.name) { label = u8(p.label); range = p.max - p.min; }
      if (t == "tone" || t == "tone2") { label += u8(" (oct)"); range = 8; }
      out.push_back({t, label, range});
    }
    return out;
  }();
  return list;
}
double targetRange(const std::string& target) {
  for (const auto& t : targetChoices()) if (t.id == target) return t.range;
  return 1;
}

// The sources a module's edit square shows.
std::vector<std::string> moduleSources(const WCoreModular::ModuleDef& m) {
  if (!m.source) return {};
  const std::string s = m.source;
  if (s == "macro") return {"macro1", "macro2", "macro3", "macro4"};
  if (s == "play") return {"velocity", "note", "modwheel"};
  return {s};
}

// A classic shape between the five names (0 Round .. 4 Hard), as the engine's basic shapes, for the screens.
double classicAt(int shape, double p) {
  switch (shape) {
    case 0: return std::sin(juce::MathConstants<double>::twoPi * p);
    case 1: { double q = p + 0.25; if (q >= 1) q -= 1; return 1 - 2 * std::fabs(2 * q - 1); }
    case 2: return p < 0.5 ? 0.8 : -0.8;
    case 3: return 1 - 2 * p;
    default: return p < 0.25 ? 0.9 : -0.5;
  }
}
double shapeAt(double w, double p) {
  const int a = std::min(3, (int)std::floor(std::clamp(w, 0.0, 4.0)));
  const double mix = std::clamp(w, 0.0, 4.0) - a;
  return classicAt(a, p) * (1 - mix) + classicAt(a + 1, p) * mix;
}
// The synth's tail: the mood's Core FX on the track (its delay feeding its reverb), -1 when the mood has none.
int tailIndex(const doc::Track& t) {
  for (size_t i = 0; i < t.effects.size(); ++i) {
    const auto& fx = t.effects[i];
    if (fx.is_object() && fx.value("name", "") == "corefx" && fx.contains(doc::kWaveMoodKey)) return (int)i;
  }
  return -1;
}
double tailValue(const doc::Track& t, const std::string& param, double def) {
  const int i = tailIndex(t);
  if (i < 0) return def;
  const auto& fx = t.effects[(size_t)i];
  return fx.contains("params") ? fx["params"].value(param, def) : def;
}
}  // namespace

// ---------------------------------------------------------------- the module list
const std::vector<WCoreModular::ModuleDef>& WCoreModular::modules() {
  // The first `primary` knobs are the module's face (the design's row of knobs); "Mais" shows the rest.
  static const std::vector<ModuleDef> list = {
      {"osc1", "Osc A", "", 0, nullptr, nullptr,
       {"osc1Pos", "unison", "uniDetune", "osc1Semi", "osc1Level", "wave", "osc1Warp", "osc1WarpAmt", "uniBlend", "uniWidth", "octave", "tune", "fm",
        "fmRatio", "osc1Route"}, 5},
      {"osc2", "Osc B", "", 0, "osc2", nullptr,
       {"osc2Pos", "osc2Fine", "osc2Semi", "osc2", "osc2Wave", "osc2Warp", "osc2WarpAmt", "osc2Route"}, 4},
      {"sub", "Sub", "", 0, nullptr, nullptr, {"subOct", "subPan", "subLevel", "subRoute"}, 3},
      {"grain", "Sample", "", 0, "grain", nullptr, {"grainPos", "grainSize", "grainPitch", "grain", "grainDensity", "grainScan", "grainScatter"}, 4},
      {"noise", "Noise", "Noise source", 0, "colour", nullptr, {"noiseColor", "noise", "noiseRoute"}, 2},
      {"filter1", "Filter 1", "", 1, "filter", nullptr, {"tone", "bite", "filterDrive", "filterMix", "filterShape"}, 4},
      {"filter2", "Filter 2", "", 1, nullptr, nullptr, {"tone2", "bite2", "filter2Drive", "filter2Mix", "filter2Shape", "filterSerial"}, 4},
      {"drive", "Drive", "Saturation", 1, nullptr, nullptr, {"drive", "driveColour", "driveMix", "crush"}, 3},
      {"env1", "ENV 1", "Amplitude", 2, nullptr, "env1", {"env1Attack", "env1Decay", "env1Sustain", "env1Release", "env1Hold"}, 4},
      {"env2", "ENV 2", "Modulation", 2, nullptr, "env2", {"env2Attack", "env2Decay", "env2Sustain", "env2Release", "env2Hold"}, 4},
      {"env3", "ENV 3", "Modulation", 2, nullptr, "env3", {"env3Attack", "env3Decay", "env3Sustain", "env3Release", "env3Hold"}, 4},
      {"lfo1", "LFO 1", "", 3, "lfo1", "lfo1", {"lfo1Rate", "lfo1Depth", "lfo1Phase", "lfo1Smooth", "lfo1Hz"}, 4},
      {"lfo2", "LFO 2", "", 3, "lfo2", "lfo2", {"lfo2Rate", "lfo2Depth", "lfo2Phase", "lfo2Smooth", "lfo2Hz"}, 4},
      {"lfo3", "LFO 3", "", 3, nullptr, "lfo3", {"lfo3Rate", "lfo3Depth", "lfo3Phase", "lfo3Smooth", "lfo3Hz"}, 4},
      {"lfo4", "LFO 4", "", 3, nullptr, "lfo4", {"lfo4Rate", "lfo4Depth", "lfo4Phase", "lfo4Smooth", "lfo4Hz"}, 4},
      {"delay", "Delay", "Delay", 4, nullptr, nullptr, {"delTime", "delFeedback", "delReturn"}, 3},
      {"reverb", "Reverb", "Reverb", 4, nullptr, nullptr, {"revDecay", "revPreDelay", "revReturn"}, 3},
      {"out", "Output", "", 5, nullptr, nullptr, {"level", "outPan"}, 2},
      {"play", "Play", "", 5, nullptr, "play", {"polyVoices", "glide", "bendRange", "mode", "legato", "glideAlways", "bend", "modWheel"}, 4},
      {"macros", "Macros", "", 5, nullptr, "macro", {"macro1", "macro2", "macro3", "macro4"}, 4}};
  return list;
}

// ---------------------------------------------------------------- the content (the tab's rows, inside the viewport)
struct WCoreModular::Content final : public juce::Component {
  explicit Content(WCoreModular& o) : owner(o) {}
  WCoreModular& owner;
  std::vector<std::unique_ptr<juce::Component>> parts;  // the cards and the matrix, in layout order
  struct Zone { int zone; juce::Rectangle<int> label; bool folded; juce::String names; };
  std::vector<Zone> zones;  // the OSC tab's rows: the vertical name (or the whole bar, minimised)
  void paint(juce::Graphics& g) override {
    g.fillAll(kBg);
    // The brief's row names: the Wave's colour on its tint (12 % over the surface), ▴ over the name turned on its side;
    // minimised, one bar with ▾, the name and what is in the row.
    for (const auto& z : zones) {
      const juce::Colour c = owner.rowColour(z.zone);
      const bool over = isMouseOver() && z.label.contains(getMouseXYRelative());
      g.setColour(kPanelC.interpolatedWith(c, over ? 0.45f : 0.35f));
      g.fillRect(z.label);
      g.setColour(kLine);
      g.drawRect(z.label);
      g.setColour(c.interpolatedWith(kTextC, 0.25f));
      if (z.folded) {
        auto r = z.label.reduced(8, 0);
        theme::drawIcon(g, "down", r.removeFromLeft(12).toFloat().withSizeKeepingCentre(12, 12));
        r.removeFromLeft(8);
        g.setFont(font(theme::kFontBody, true).withExtraKerningFactor(0.08f));
        g.drawText(u8(kZoneNames[z.zone]), r.removeFromLeft(90), juce::Justification::centredLeft);
        g.setColour(kMutedC);
        g.setFont(font(theme::kFontSmall));
        g.drawText(z.names, r, juce::Justification::centredLeft);
        continue;
      }
      auto r = z.label;
      theme::drawIcon(g, "up", r.removeFromTop(20).toFloat().withSizeKeepingCentre(11, 11));
      juce::Graphics::ScopedSaveState state(g);
      const auto centre = r.getCentre().toFloat();
      g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, centre.x, centre.y));
      g.setFont(font(theme::kFontSmall, true).withExtraKerningFactor(0.1f));
      g.drawText(u8(kZoneNames[z.zone]), juce::Rectangle<float>((float)r.getHeight(), (float)r.getWidth()).withCentre(centre),
                 juce::Justification::centred);
    }
  }
  // Each row framed in its colour, 2 px, over the cards' own thin lines.
  void paintOverChildren(juce::Graphics& g) override {
    for (const auto& z : zones) {
      auto r = z.label;
      if (!z.folded) r.setRight(getWidth() - kPad);
      g.setColour(owner.rowColour(z.zone));
      g.drawRect(r, 2);
    }
  }
  void mouseMove(const juce::MouseEvent&) override { repaint(); }
  void mouseExit(const juce::MouseEvent&) override { repaint(); }
  void mouseUp(const juce::MouseEvent& e) override {
    for (const auto& z : zones)
      if (z.label.contains(e.getPosition())) { owner.foldZone(z.zone, !z.folded); return; }
  }
};

// ---------------------------------------------------------------- small header buttons
namespace {
// The module's light: on = it plays; off = left out, its knobs kept (the section lights).
class PowerLight final : public juce::Button {
public:
  PowerLight() : juce::Button("power") { setWantsKeyboardFocus(false); }
  juce::Colour colour = theme::kAccent;
  void paintButton(juce::Graphics& g, bool over, bool) override {
    // On / bypass as the brief: a lighter tone of the colour, the circle full when on, only its outline when off.
    const auto c = juce::Rectangle<float>(9.0f, 9.0f).withCentre(getLocalBounds().toFloat().getCentre());
    g.setColour(colour.interpolatedWith(juce::Colours::white, over ? 0.5f : 0.35f));
    if (getToggleState()) g.fillEllipse(c);
    g.drawEllipse(c.reduced(0.75f), 1.5f);
  }
};
// On / off as a switch (not a knob): a square track and a square thumb, the track's colour when on.
class SwitchButton final : public juce::Button {
public:
  SwitchButton() : juce::Button("switch") { setWantsKeyboardFocus(false); setClickingTogglesState(false); }
  juce::Colour colour = theme::kAccent;
  void paintButton(juce::Graphics& g, bool over, bool) override {
    auto t = juce::Rectangle<float>(34.0f, 16.0f).withCentre(getLocalBounds().toFloat().getCentre());
    const bool on = getToggleState();
    g.setColour(on ? colour.withAlpha(over ? 0.55f : 0.4f) : (over ? theme::kControlHover : kInner));
    g.fillRect(t);
    g.setColour(on ? colour : kLine);
    g.drawRect(t, 1.0f);
    g.setColour(on ? kTextC : kMutedC);
    g.fillRect((on ? t.removeFromRight(16.0f) : t.removeFromLeft(16.0f)).reduced(3.0f));
  }
};
// A principal (Wave 1..4): a chip with its colour dot; dark when off, light when chosen, the mouse inverts it.
class WaveChip final : public juce::Button {
public:
  WaveChip() : juce::Button("wave") { setWantsKeyboardFocus(false); setClickingTogglesState(false); }
  juce::Colour dot = theme::kAccent;
  bool exists = true;
  std::function<void()> onMenu;  // right-click: its colour, remove it
  void clicked(const juce::ModifierKeys& m) override {
    if (m.isPopupMenu()) { if (exists && onMenu) onMenu(); return; }
    juce::Button::clicked(m);
  }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    const bool light = getToggleState() != (over || down);
    g.setColour(light ? theme::kBlueLight : theme::kControl);
    g.fillRect(r);
    g.setColour(light ? theme::kBlueLight : kLine);
    g.drawRect(r, 1.0f);
    auto t = getLocalBounds().reduced(10, 0);
    if (exists) {
      g.setColour(dot);
      g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({(float)t.getX() + 4.0f, r.getCentreY()}));
      t.removeFromLeft(14);
    }
    g.setColour(light ? theme::kBackground : kTextC);
    g.setFont(font(theme::kFontStrong));
    g.drawText(getButtonText(), t, juce::Justification::centredLeft);
  }
};
}  // namespace

// ---------------------------------------------------------------- a module
// The design's card: a dot (its light, when it has one) and the name; on the right, its type, big enough to read and,
// where the type is a choice (table, shape, filter mode, LFO shape), the list itself; under it the screen and a row
// of knobs, each with its name over it and its value in a box under it. "Mais" shows the module's other controls.
class WCoreModular::Card final : public juce::Component {
public:
  Card(WCoreModular& o, const ModuleDef& m) : owner_(o), def_(m) {
    const doc::Track* t = owner_.track();
    const int layer = owner_.layer();
    const juce::Colour c = owner_.layerColour(owner_.layer());
    const std::string id = def_.id;
    more_ = owner_.more_.count(id) > 0;
    if (def_.section) {
      light_.colour = c;
      light_.setClickingTogglesState(false);
      light_.setTooltip(u8("Ligado: toca. Desligado: fica de fora, os bot\xc3\xb5" "es ficam como est\xc3\xa3o."));
      light_.onClick = [this] {
        const doc::Track* tr = owner_.track();
        if (!tr) return;
        const bool on = !doc::waveSectionOn(*tr, owner_.layer(), def_.section);
        const std::string target = owner_.trackId_;
        const int l = owner_.layer();
        const std::string section = def_.section;
        owner_.host_.applyEdit([&](doc::Document& d) { doc::setWaveSectionOn(d, target, l, section, on); },
                               u8(def_.name) + (on ? u8(" on") : u8(" off")));
      };
      addAndMakeVisible(light_);
    }
    // The type, in the header: a list where it is a choice.
    if (const char* tp = typeParam(id)) {
      const std::string param = doc::waveLayerParam(layer, tp);
      if (const doc::InstrumentParamInfo* p = t ? doc::findInstrumentParam(*t, param) : nullptr) {
        typeInfo_ = *p;
        typeParam_ = param;
        for (int v = (int)p->min; v <= (int)p->max; ++v) type_.addItem(waveValueText(*p, v), v - (int)p->min + 1);
        type_.setSelectedId((int)std::lround(doc::instrumentParamValue(*t, *p)) - (int)p->min + 1, juce::dontSendNotification);
        type_.onChange = [this] { owner_.setParam(typeParam_, (double)(type_.getSelectedId() - 1) + typeInfo_.min); };
        hasType_ = true;
      }
    }
    // The Sample module's sound: the file's name, centred, as in the design.
    if (id == "grain") {
      const doc::SampleRef* z = t ? doc::waveLayerSound(*t, layer) : nullptr;
      sound_.setButtonText(z ? u8(z->name) : u8("Wave"));
      sound_.setColour(juce::TextButton::buttonColourId, kBg);
      sound_.setColour(juce::TextButton::textColourOffId, kTextC);
      sound_.setTooltip(u8("O som de onde os gr\xc3\xa3os s\xc3\xa3o cortados: um sample da biblioteca, ou a onda do synth."));
      sound_.onClick = [this] { owner_.chooseGrainSound(&sound_); };
      sound_.setWantsKeyboardFocus(false);
      addAndMakeVisible(sound_);
    }
    // An LFO: its shape is the type list; the mode and the sync sit over its knobs.
    if (id.rfind("lfo", 0) == 0) {
      lfo_ = def_.id[3] - '1';
      const doc::json lj = lfoJson();
      static const char* shapes[] = {"sine", "triangle", "saw", "square", "random", "points"};
      static const char* shapeNames[] = {"Sine", "Triangle", "Saw", "Square", "Random", "Drawn"};
      for (int i = 0; i < 6; ++i) type_.addItem(u8(shapeNames[i]), i + 1);
      const std::string sh = lj.value("shape", "sine");
      for (int i = 0; i < 6; ++i) if (sh == shapes[i]) type_.setSelectedId(i + 1, juce::dontSendNotification);
      type_.onChange = [this] {
        const int i = type_.getSelectedId() - 1;
        if (i >= 0 && i < 6) owner_.setLfo(lfo_, {{"shape", shapes[i]}});
      };
      hasType_ = true;
      // Mode and Sync are on the header's right-click (the brief's LFO card: the shape, the screen, four knobs).
    }
    type_.setWantsKeyboardFocus(false);
    // The controls: knobs, lists and switches; the first `primary` are the face, the rest behind "Mais".
    const bool synced = lfo_ < 0 || lfoJson().value("sync", true);
    // FX: the tail's own knobs (the mood's Core FX on the track).
    if (isTailModule(id) && t)
      for (const char* name : def_.knobs)
        if (const doc::EffectParamInfo* fp = tailParam(name)) {
          const doc::InstrumentParamInfo info{fp->name, fp->label, fp->def, fp->min, fp->max, fp->unit, fp->step, fp->logScale, fp->automatable};
          const doc::EffectParamInfo copy = *fp;
          addKnob(*t, info, "", std::string(name).find("Return") != std::string::npos ? u8("Mix") : u8(fp->label), fp->hint ? u8(fp->hint) : juce::String(), c, name, [copy](double v) { return tailText(copy, v); });
        }
    for (size_t i = 0; i < (isTailModule(id) ? 0 : def_.knobs.size()); ++i) {
      std::string name = def_.knobs[i];
      bool primary = (int)i < def_.primary;
      // An LFO shows Rate (in beats) when it follows the tempo, Hz when it is free.
      if (lfo_ >= 0 && name.size() > 5 && name.substr(4) == "Rate" && !synced) continue;
      if (lfo_ >= 0 && name.size() > 5 && name.substr(4) == "Hz") { if (synced) primary = false; else primary = true; }
      if (!primary && !more_) { hasMore_ = true; continue; }
      if (!primary) hasMore_ = true;
      const std::string param = name == "mode" ? name : doc::waveLayerParam(layer, name.c_str());  // Mono / Poly: the synth's
      const doc::InstrumentParamInfo* p = t ? doc::findInstrumentParam(*t, param) : nullptr;
      if (!p) continue;
      if (name == "mode") {
        addKnob(*t, *p, param, u8("Mode"), u8("Mono: one note at a time (lead, bass). Poly: chords (pad)."), c, {},
                [](double v) { return v >= 0.5 ? juce::String("Poly") : juce::String("Mono"); });
        continue;
      }
      juce::String label = u8(p->label), tip;
      for (const auto& l : doc::waveLayers())
        for (const auto& kk : l.knobs) if (std::string(kk.param) == name) { label = u8(kk.name); tip = u8(kk.info); }
      if (isChoice(name) || isSwitch(name)) addOption(*t, *p, param, label, tip, c, isSwitch(name));
      else addKnob(*t, *p, param, label, tip, c);
    }
    if (!isTailModule(id) && def_.knobs.size() > (size_t)def_.primary) hasMore_ = true;
    // The module's other controls ("All controls" on the header's right-click), not a button on its face.
    // Output: Wash and Dry (the psytrance wash): how much goes into the tail (the mood's Core FX, its delay feeding
    // its reverb) and the dry sound's level. Shown when the mood has a tail.
    if (id == "out" && t && tailIndex(*t) >= 0) hasMore_ = true;
    if (id == "out" && t && tailIndex(*t) >= 0 && more_) {
      struct TailKnob { const char* fx; const char* name; const char* tip; double def; };
      for (const TailKnob& tk : {TailKnob{"amount", "Wash", "Quanto do som entra na cauda (delay a alimentar o reverb)", 0.5},
                                 TailKnob{"dry", "Dry", "O volume do som original, por baixo da cauda", 1.0}}) {
        const doc::InstrumentParamInfo info{tk.fx, tk.name, tk.def, 0, 1, "", 0, false, true};
        addKnob(*t, info, "", u8(tk.name), u8(tk.tip), c, tk.fx, [](double v) { return juce::String(juce::roundToInt(v * 100)) + " %"; });
      }
    }
    // Its links, while the edit square is on.
    if (def_.source && owner_.editing_ == def_.id) {
      const auto sources = moduleSources(def_);
      const doc::json links = owner_.layerSynth().value("links", doc::json::array());
      for (const auto& l : links) {
        const std::string src = l.value("source", "");
        if (std::find(sources.begin(), sources.end(), src) == sources.end()) continue;
        rows_.push_back(std::make_unique<LinkRow>(owner_, l, sources.size() > 1, c));
        addAndMakeVisible(*rows_.back());
      }
      add_.setButtonText(u8("+ Liga\xc3\xa7\xc3\xa3o"));
      add_.setTooltip(u8("Liga esta fonte a um bot\xc3\xa3o, \xc3\xa0 afina\xc3\xa7\xc3\xa3o, ao volume, ao pan ou ao detune."));
      add_.onClick = [this, sources] {
        if (sources.size() == 1) { owner_.addLink(sources[0]); return; }
        juce::PopupMenu menu;
        for (size_t i = 0; i < sources.size(); ++i) menu.addItem((int)i + 1, sourceLabel(sources[i]));
        juce::Component::SafePointer<Card> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_), [safe, sources](int r) {
          if (safe && r > 0) safe->owner_.addLink(sources[(size_t)r - 1]);
        });
      };
      add_.setWantsKeyboardFocus(false);
      addAndMakeVisible(add_);
    }
    refresh();
  }

  // The narrowest the module can be: its knobs at the brief's smallest step. The row shares its width by the brief's
  // proportions (Sources 1.2 / 1.1 / .78 / 1.05 / .78, the others in thirds, Global .5 / 1 / 1).
  int minimumWidth() const { return std::max(150, (int)std::min<size_t>(knobs_.size(), (size_t)std::max(1, def_.primary)) * kKnobMin + 2 * kBodyPad); }
  // The height the module wants at a width (the row then gives every card the row's height).
  int preferredHeight(int width) const {
    int h = kCardHead + kBodyPad;
    if (hasScreen()) h += kScreenH + 6;
    if (sound_.isVisible()) h += kFileH + 4;
    h += (int)options_.size() * (kOptionH + 2);
    h += knobRows(width) * kKnobH + kBodyPad;
    if (def_.source && owner_.editing_ == def_.id) h += 20 + (int)rows_.size() * kLinkH + kSelectH + 8;
    return h;
  }
  // The brief draws a screen on the sources, the filters, Drive, the envelopes and the LFOs; FX and Global are knobs.
  bool hasScreen() const {
    const std::string id = def_.id;
    return !(isTailModule(id) || id == "out" || id == "play" || id == "macros");
  }

  // Values from the document (undo, another window): no notifications, no history.
  void refresh() {
    const doc::Track* t = owner_.track();
    if (!t) return;
    for (auto& k : knobs_) {
      if (k.slider->isMouseButtonDown()) continue;
      k.slider->setValue(k.fx.empty() ? doc::instrumentParamValue(*t, k.info) : tailValue(*t, k.fx, k.info.def), juce::dontSendNotification);
      k.slider->updateText();
    }
    for (auto& o : options_) {
      const double v = doc::instrumentParamValue(*t, o.info);
      if (o.toggle) o.toggle->setToggleState(v >= 0.5, juce::dontSendNotification);
      if (o.list) o.list->setSelectedId((int)std::lround(v) - (int)o.info.min + 1, juce::dontSendNotification);
    }
    if (!typeParam_.empty())
      type_.setSelectedId((int)std::lround(doc::instrumentParamValue(*t, typeInfo_)) - (int)typeInfo_.min + 1, juce::dontSendNotification);
    if (def_.section) light_.setToggleState(doc::waveSectionOn(*t, owner_.layer(), def_.section), juce::dontSendNotification);
    const doc::json links = owner_.layerSynth().value("links", doc::json::array());
    for (auto& r : rows_) r->refresh(links);
    repaint();
  }

  void resized() override {
    auto b = getLocalBounds();
    auto head = b.removeFromTop(kCardHead).reduced(8, 3);
    if (def_.section) light_.setBounds(head.removeFromLeft(16));
    else head.removeFromLeft(16);
    head.removeFromLeft(nameWidth());
    typeArea_ = head;
    b = b.reduced(kBodyPad, 0);
    b.removeFromTop(kBodyPad);
    screen_ = {};
    if (hasScreen()) { screen_ = b.removeFromTop(kScreenH); b.removeFromTop(6); }
    if (sound_.isVisible()) { sound_.setBounds(b.removeFromTop(kFileH)); b.removeFromTop(4); }
    for (auto& o : options_) {
      auto r = b.removeFromTop(kOptionH).reduced(0, 2);
      b.removeFromTop(2);
      o.label->setBounds(r.removeFromLeft(std::min(110, r.getWidth() / 2)));
      if (o.toggle) o.toggle->setBounds(r.removeFromLeft(40));
      if (o.list) o.list->setBounds(r.removeFromLeft(std::min(150, r.getWidth())));
    }
    // The knobs share the card's width evenly, as the brief's grid does: name over the dial, the value in a box under it.
    const int cols = columns(getWidth());
    const int rows = knobRows(getWidth());
    auto grid = b.removeFromTop(rows * kKnobH);
    const float cw = (float)grid.getWidth() / (float)std::max(1, cols);
    for (size_t i = 0; i < knobs_.size(); ++i) {
      const int col = (int)i % cols, row = (int)i / cols;
      const int x0 = grid.getX() + juce::roundToInt(col * cw), x1 = grid.getX() + juce::roundToInt((col + 1) * cw);
      auto cell = juce::Rectangle<int>(x0, grid.getY() + row * kKnobH, x1 - x0, kKnobH);
      knobs_[i].label->setBounds(cell.removeFromTop(16));
      knobs_[i].slider->setBounds(cell.withSizeKeepingCentre(std::min(cell.getWidth() - 4, 72), cell.getHeight()));
    }
    if (def_.source && owner_.editing_ == def_.id) {
      b.removeFromTop(20);
      for (auto& r : rows_) r->setBounds(b.removeFromTop(kLinkH).reduced(0, 1));
      add_.setBounds(b.removeFromTop(kSelectH).removeFromLeft(120));
    }
  }

  void paint(juce::Graphics& g) override {
    const juce::Colour c = owner_.layerColour(owner_.layer());
    const doc::Track* t = owner_.track();
    const bool on = !def_.section || !t || doc::waveSectionOn(*t, owner_.layer(), def_.section);
    const float alpha = on ? 1.0f : 0.7f;
    // The brief's module: the body, the header in the Wave's colour (23 % over the surface), thin lines, square.
    const auto r = getLocalBounds();
    g.setColour(kPanelC.withMultipliedAlpha(alpha));
    g.fillRect(r);
    auto head = r.withHeight(kCardHead);
    g.setColour(kPanelC.interpolatedWith(owner_.rowColour(def_.zone), 0.35f).withMultipliedAlpha(alpha));
    g.fillRect(head);
    g.setColour(kLine);
    g.fillRect(head.getX(), head.getBottom() - 1, head.getWidth(), 1);
    g.drawRect(r);
    if (owner_.editing_ == def_.id) {  // the module whose links are open: the 2 px line in the colour
      g.setColour(c);
      g.drawRect(r, 2);
    }
    auto hd = head.reduced(8, 0);
    if (!def_.section) {  // no light: the brief's dot
      g.setColour(c.withAlpha(on ? 1.0f : 0.4f));
      g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({(float)hd.getX() + 6.0f, (float)hd.getCentreY()}));
    }
    hd.removeFromLeft(16);
    g.setColour(on ? owner_.rowColour(def_.zone).interpolatedWith(kTextC, 0.3f) : kDimC);
    g.setFont(nameFont());
    g.drawText(u8(def_.name).toUpperCase(), hd.removeFromLeft(nameWidth()), juce::Justification::centredLeft);
    // The type in words when it is not a list (Drive: Saturation, ENV 1: Amplitude...).
    // The type as the brief's small text on the right; where it is a choice (table, shape, filter mode), a click
    // there opens its list.
    const juce::String typeText = hasType_ ? type_.getText() : u8(def_.kicker);
    if (typeText.isNotEmpty()) {
      const bool over = hasType_ && isMouseOver() && typeArea_.contains(getMouseXYRelative());
      g.setColour(!on ? kDimC : over ? kTextC : owner_.rowColour(def_.zone).interpolatedWith(kTextC, 0.45f));
      g.setFont(font(theme::kFontBody));
      g.drawText(typeText, typeArea_, juce::Justification::centredRight);
    }
    if (hasScreen()) {
      // The screen: the dark field, a fine grid (ten columns, two lines), the zero line, the shape filled under its line.
      const auto scr = screen_;
      g.setColour(kBg);
      g.fillRect(scr);
      g.setColour(kLine.withAlpha(0.55f));
      for (int k = 1; k < 10; ++k) g.drawVerticalLine(scr.getX() + scr.getWidth() * k / 10, (float)scr.getY(), (float)scr.getBottom());
      g.drawHorizontalLine(scr.getY() + scr.getHeight() / 4, (float)scr.getX(), (float)scr.getRight());
      g.drawHorizontalLine(scr.getY() + scr.getHeight() * 3 / 4, (float)scr.getX(), (float)scr.getRight());
      const auto inner = scr.toFloat().reduced(1.0f, 4.0f);
      const std::string id = def_.id;
      const bool fromBottom = id.rfind("env", 0) == 0 || id == "filter1" || id == "filter2";
      const float base = fromBottom ? inner.getBottom() : inner.getCentreY();
      g.setColour(theme::kBlueBase);
      g.drawHorizontalLine(juce::roundToInt(base), inner.getX(), inner.getRight());
      juce::Path p;
      picture(p, inner);
      if (!p.isEmpty()) {
        juce::Path fill(p);
        const auto pb = p.getBounds();
        fill.lineTo(pb.getRight(), base);
        fill.lineTo(pb.getX(), base);
        fill.closeSubPath();
        g.setColour(c.withAlpha(on ? 0.14f : 0.06f));
        g.fillPath(fill);
        g.setColour(c.withAlpha(on ? 0.85f : 0.3f));
        g.strokePath(p, juce::PathStrokeType(1.4f));
      }
      g.setColour(kLine);
      g.drawRect(scr);
    }
    // Each value in its small box under the knob (the brief's value box), drawn under the knob's text.
    for (const auto& k : knobs_) {
      const auto sb = k.slider->getBounds();
      const auto box = juce::Rectangle<int>(std::min(sb.getWidth() - 2, 66), 20).withCentre({sb.getCentreX(), sb.getBottom() - 10});
      g.setColour(kBg);
      g.fillRect(box);
      g.setColour(kLine);
      g.drawRect(box);
    }
    if (def_.source && owner_.editing_ == def_.id && !rows_.empty()) {
      const int y = rows_.front()->getY() - 18;
      g.setColour(kMutedC);
      g.setFont(font(theme::kFontSmall, true));
      g.drawText(u8("LINKS \xc2\xb7 ") + juce::String((int)rows_.size()), juce::Rectangle<int>(kBodyPad, y, getWidth() - 2 * kBodyPad, 16),
                 juce::Justification::centredLeft);
    }
  }

  // Right-click: what the brief leaves off the face (all the controls, the links, an LFO's mode and sync).
  void mouseMove(const juce::MouseEvent&) override { if (hasType_) repaint(typeArea_); }
  void mouseExit(const juce::MouseEvent&) override { if (hasType_) repaint(typeArea_); }
  void mouseUp(const juce::MouseEvent& e) override {
    if (!e.mods.isPopupMenu()) {
      if (!hasType_ || !typeArea_.contains(e.getPosition())) return;
      juce::PopupMenu list;
      for (int i = 0; i < type_.getNumItems(); ++i)
        list.addItem(type_.getItemId(i), type_.getItemText(i), true, type_.getItemId(i) == type_.getSelectedId());
      juce::Component::SafePointer<Card> safe(this);
      list.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(typeArea_)), [safe](int r) {
        if (safe && r > 0) { safe->type_.setSelectedId(r, juce::sendNotificationSync); safe->repaint(); }
      });
      return;
    }
    juce::PopupMenu menu;
    if (hasMore_) menu.addItem(1, u8("All controls"), true, more_);
    if (def_.source) menu.addItem(2, u8("Its links"), true, owner_.editing_ == def_.id);
    if (lfo_ >= 0) {
      const doc::json lj = lfoJson();
      menu.addSeparator();
      const std::string md = lj.value("mode", "song");
      menu.addItem(10, u8("Runs with the song"), true, md == "song");
      menu.addItem(11, u8("Restarts on each note"), true, md == "note");
      menu.addItem(12, u8("Plays once per note"), true, md == "once");
      menu.addItem(13, u8("Sync to the tempo"), true, lj.value("sync", true));
    }
    if (menu.getNumItems() == 0) return;
    juce::Component::SafePointer<Card> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea({e.getScreenX(), e.getScreenY(), 1, 1}), [safe](int r) {
      if (!safe || r <= 0) return;
      Card& c = *safe;
      static const char* modes[] = {"song", "note", "once"};
      if (r == 1) c.owner_.showMore(c.def_.id, !c.more_);
      else if (r == 2) c.owner_.edit(c.owner_.editing_ == c.def_.id ? std::string() : std::string(c.def_.id));
      else if (r >= 10 && r <= 12) c.owner_.setLfo(c.lfo_, {{"mode", modes[r - 10]}});
      else if (r == 13) c.owner_.setLfo(c.lfo_, {{"sync", !c.lfoJson().value("sync", true)}});
    });
  }

private:
  struct Knob {
    std::string param;
    std::string fx;  // a knob of the synth's tail (Wash, Dry): the Core FX parameter; "" = the synth's own
    doc::InstrumentParamInfo info{};
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<juce::Slider> slider;
  };
  // A choice (a list) or an on / off (a switch), with its name on the left.
  struct Option {
    doc::InstrumentParamInfo info{};
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<juce::ComboBox> list;
    std::unique_ptr<SwitchButton> toggle;
  };
  void addKnob(const doc::Track& t, const doc::InstrumentParamInfo& p, const std::string& param, const juce::String& label,
               const juce::String& tip, juce::Colour c, const std::string& fx = {},
               std::function<juce::String(double)> text = {}) {
    Knob k;
    k.param = param;
    k.fx = fx;
    k.info = p;
    k.label = std::make_unique<juce::Label>();
    k.label->setText(label, juce::dontSendNotification);
    k.label->setTooltip(tip);
    k.label->setFont(font(theme::kFontBody));
    k.label->setJustificationType(juce::Justification::centred);
    k.label->setColour(juce::Label::textColourId, owner_.rowColour(def_.zone).interpolatedWith(kTextC, 0.4f));
    k.label->setInterceptsMouseClicks(false, false);
    k.slider = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    auto& s = *k.slider;
    // The value in a box under the knob, as in the design.
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 64, 20);
    s.setColour(juce::Slider::textBoxOutlineColourId, kLine);
    s.setColour(juce::Slider::textBoxBackgroundColourId, kInner);
    s.setColour(juce::Slider::textBoxTextColourId, owner_.rowColour(def_.zone).interpolatedWith(kTextC, 0.55f));
    s.setColour(juce::Slider::rotarySliderFillColourId, owner_.rowColour(def_.zone));  // the row's colour (Diogo 2026-11-09)
    s.setColour(juce::Slider::rotarySliderOutlineColourId, kLine);
    s.setColour(juce::Slider::thumbColourId, kTextC);
    s.setRange(p.min, p.max, p.step > 0 ? p.step : 0.0);
    if (p.logScale) s.setSkewFactorFromMidPoint(std::sqrt(std::max(1e-6, p.min) * p.max));
    s.setDoubleClickReturnValue(true, p.def);
    const doc::InstrumentParamInfo info = p;
    if (text) s.textFromValueFunction = std::move(text);
    else s.textFromValueFunction = [info](double v) { return waveValueText(info, v); };
    s.setValue(fx.empty() ? doc::instrumentParamValue(t, p) : tailValue(t, fx, p.def), juce::dontSendNotification);
    s.updateText();
    s.setWantsKeyboardFocus(false);
    juce::Slider* sp = &s;
    s.onDragStart = [this, label] { owner_.host_.startGesture(u8("Synth ") + label); owner_.dragging_ = true; };
    if (fx.empty()) s.onValueChange = [this, sp, param] { owner_.setParam(param, sp->getValue()); };
    else s.onValueChange = [this, sp, fx] { owner_.setTail(fx, sp->getValue()); };
    s.onDragEnd = [this] { if (owner_.dragging_) owner_.host_.finishGesture(); owner_.dragging_ = false; };
    addAndMakeVisible(*k.label);
    addAndMakeVisible(s);
    knobs_.push_back(std::move(k));
  }
  void addOption(const doc::Track& t, const doc::InstrumentParamInfo& p, const std::string& param, const juce::String& label,
                 const juce::String& tip, juce::Colour c, bool isOnOff) {
    Option o;
    o.info = p;
    o.label = std::make_unique<juce::Label>();
    o.label->setText(label, juce::dontSendNotification);
    o.label->setTooltip(tip);
    o.label->setFont(font(theme::kFontBody));
    o.label->setColour(juce::Label::textColourId, owner_.rowColour(def_.zone).interpolatedWith(kTextC, 0.4f));
    addAndMakeVisible(*o.label);
    const double v = doc::instrumentParamValue(t, p);
    if (isOnOff) {
      o.toggle = std::make_unique<SwitchButton>();
      o.toggle->colour = c;
      o.toggle->setToggleState(v >= 0.5, juce::dontSendNotification);
      SwitchButton* sw = o.toggle.get();
      o.toggle->onClick = [this, sw, param] { owner_.setParam(param, sw->getToggleState() ? 0.0 : 1.0); };
      addAndMakeVisible(*o.toggle);
    } else {
      o.list = std::make_unique<juce::ComboBox>();
      for (int i = (int)p.min; i <= (int)p.max; ++i) o.list->addItem(waveValueText(p, i), i - (int)p.min + 1);
      o.list->setSelectedId((int)std::lround(v) - (int)p.min + 1, juce::dontSendNotification);
      juce::ComboBox* box = o.list.get();
      const double lo = p.min;
      o.list->onChange = [this, box, param, lo] { owner_.setParam(param, (double)(box->getSelectedId() - 1) + lo); };
      o.list->setWantsKeyboardFocus(false);
      addAndMakeVisible(*o.list);
    }
    options_.push_back(std::move(o));
  }
  int columns(int width) const {
    const int n = std::max(1, (int)knobs_.size());
    const int fit = std::max(1, (width - 2 * kBodyPad) / kKnobMin);
    return std::max(1, std::min({n, fit, std::max(kMaxCols, def_.primary)}));
  }
  int knobRows(int width) const {
    if (knobs_.empty()) return 0;
    const int cols = columns(width);
    return ((int)knobs_.size() + cols - 1) / cols;
  }
  static juce::Font nameFont() { return font(theme::kFontStrong, true).withExtraKerningFactor(0.04f); }
  int nameWidth() const { return juce::GlyphArrangement::getStringWidthInt(nameFont(), u8(def_.name).toUpperCase()) + 12; }
  // One link: its source (several sources: Macros, Tocar), where it goes, how much, on, and the ✕.
  class LinkRow final : public juce::Component {
  public:
    LinkRow(WCoreModular& o, const doc::json& link, bool showSource, juce::Colour c) : owner_(o), id_(link.value("id", "")) {
      if (showSource) {
        source_.setText(sourceLabel(link.value("source", "")), juce::dontSendNotification);
        source_.setFont(font(theme::kFontSmall));
        source_.setColour(juce::Label::textColourId, kMutedC);
        addAndMakeVisible(source_);
      }
      const auto& choices = targetChoices();
      for (size_t i = 0; i < choices.size(); ++i) target_.addItem(choices[i].label, (int)i + 1);
      const std::string tg = link.value("target", "");
      for (size_t i = 0; i < choices.size(); ++i) if (choices[i].id == tg) target_.setSelectedId((int)i + 1, juce::dontSendNotification);
      target_.onChange = [this] {
        const int i = target_.getSelectedId() - 1;
        if (i >= 0 && i < (int)targetChoices().size()) owner_.setLink(id_, {{"target", targetChoices()[(size_t)i].id}});
      };
      const double range = targetRange(tg);
      amount_.setSliderStyle(juce::Slider::LinearBar);
      amount_.setTextBoxStyle(juce::Slider::TextBoxLeft, true, 50, kLinkH - 4);
      amount_.setRange(-range, range, 0.0);
      amount_.setDoubleClickReturnValue(true, 0.0);
      amount_.setColour(juce::Slider::trackColourId, c.withAlpha(0.6f));
      amount_.setColour(juce::Slider::backgroundColourId, kInner);
      amount_.setColour(juce::Slider::textBoxTextColourId, kTextC);
      amount_.textFromValueFunction = [](double v) { return (v > 0 ? "+" : "") + juce::String(v, 2); };
      amount_.setValue(link.value("amount", 0.0), juce::dontSendNotification);
      amount_.onDragStart = [this] { owner_.host_.startGesture(u8("Synth: liga\xc3\xa7\xc3\xa3o")); owner_.dragging_ = true; };
      amount_.onValueChange = [this] { owner_.setLink(id_, {{"amount", amount_.getValue()}}); };
      amount_.onDragEnd = [this] { if (owner_.dragging_) owner_.host_.finishGesture(); owner_.dragging_ = false; };
      on_.colour = c;
      on_.setToggleState(link.value("on", true), juce::dontSendNotification);
      on_.setTooltip(u8("Liga\xc3\xa7\xc3\xa3o ligada / desligada (fica guardada)"));
      on_.onClick = [this] { owner_.setLink(id_, {{"on", !on_.getToggleState()}}); };
      remove_.setButtonText(u8("\xc3\x97"));
      remove_.setTooltip(u8("Apagar esta liga\xc3\xa7\xc3\xa3o"));
      remove_.onClick = [this] { owner_.removeLink(id_); };
      for (juce::Component* x : std::initializer_list<juce::Component*>{&target_, &amount_, &on_, &remove_}) { x->setWantsKeyboardFocus(false); addAndMakeVisible(x); }
    }
    void refresh(const doc::json& links) {
      for (const auto& l : links)
        if (l.value("id", "") == id_) {
          if (!amount_.isMouseButtonDown()) amount_.setValue(l.value("amount", 0.0), juce::dontSendNotification);
          on_.setToggleState(l.value("on", true), juce::dontSendNotification);
        }
    }
    void resized() override {
      auto b = getLocalBounds();
      remove_.setBounds(b.removeFromRight(22));
      on_.setBounds(b.removeFromRight(20));
      if (source_.isVisible()) source_.setBounds(b.removeFromLeft(62));
      target_.setBounds(b.removeFromLeft(b.getWidth() * 11 / 20));
      b.removeFromLeft(4);
      amount_.setBounds(b);
    }

  private:
    WCoreModular& owner_;
    std::string id_;
    juce::Label source_;
    juce::ComboBox target_;
    juce::Slider amount_;
    PowerLight on_;
    juce::TextButton remove_;
  };

  doc::json lfoJson() const {
    const doc::json lfos = owner_.layerSynth().value("lfos", doc::json::array());
    return lfo_ >= 0 && lfo_ < (int)lfos.size() ? lfos[(size_t)lfo_] : doc::json{{"shape", "sine"}, {"mode", "song"}, {"sync", true}};
  }
  double knob(const char* name) const {
    const std::string param = doc::waveLayerParam(owner_.layer(), name);
    for (const auto& k : knobs_) if (k.param == param) return k.slider->getValue();
    const doc::Track* t = owner_.track();
    const doc::InstrumentParamInfo* p = t ? doc::findInstrumentParam(*t, param) : nullptr;
    return p ? doc::instrumentParamValue(*t, *p) : 0.0;
  }
  // The small picture: the wave, the envelope, the LFO's shape or the filter's curve.
  void picture(juce::Path& p, juce::Rectangle<float> r) const {
    const std::string id = def_.id;
    const float x0 = r.getX(), w = r.getWidth(), yc = r.getCentreY(), hh = r.getHeight() / 2;
    auto plot = [&](auto f) {
      for (int i = 0; i <= 120; ++i) {
        const double x = i / 120.0;
        const juce::Point<float> pt(x0 + (float)x * w, yc - (float)std::clamp(f(x), -1.0, 1.0) * hh);
        if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
      }
    };
    if (id == "osc1" || id == "osc2") {
      const double wv = knob(id == "osc1" ? "wave" : "osc2Wave");
      plot([&](double x) { double q = x * 3; return 0.9 * shapeAt(wv, q - std::floor(q)); });
    } else if (id == "sub") {
      const int s = (int)std::lround(knob("subShape"));
      static const int shapes[6] = {3, 0, 1, 3, 2, 4};
      const double wv = s == 0 ? knob("wave") : -1;
      plot([&](double x) { double q = x * 2; q -= std::floor(q); return 0.9 * (s == 0 ? shapeAt(wv, q) : classicAt(shapes[s], q)); });
    } else if (id == "noise" || id == "grain") {
      uint32_t seed = 0x9E3779B9u;
      const double amount = id == "noise" ? knob("noise") : knob("grain");
      plot([&](double) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return amount * ((seed & 0xffff) / 32768.0 - 1); });
    } else if (id == "filter1" || id == "filter2") {
      const bool two = id == "filter2";
      const double fc = knob(two ? "tone2" : "tone"), res = knob(two ? "bite2" : "bite");
      const int mode = (int)std::lround(knob(two ? "filter2Mode" : "filterMode"));
      const bool high = mode == 3 || mode == 6 || mode == 8 || mode == 14, band = mode == 2 || mode == 7;
      const int order = mode == 1 || mode == 8 ? 2 : mode == 5 || mode == 6 ? 1 : 4;
      plot([&](double x) {
        const double f = 20 * std::pow(1000.0, x), ratio = f / std::max(1.0, fc);
        double mag = high ? std::pow(ratio, order) / (1 + std::pow(ratio, order)) : band ? 1 / (1 + std::pow(std::log2(ratio) * 1.5, 2))
                                                                                        : 1 / (1 + std::pow(ratio, order));
        mag *= 1 + res * 2.5 * std::exp(-std::pow(std::log2(ratio) * 3, 2));
        return std::clamp(1.6 * mag - 0.8, -1.0, 1.0);
      });
    } else if (id.rfind("env", 0) == 0) {
      const std::string e = id;
      const double a = knob((e + "Attack").c_str()), h = knob((e + "Hold").c_str()), d = knob((e + "Decay").c_str()),
                   s = knob((e + "Sustain").c_str()), rl = knob((e + "Release").c_str());
      const double hold = 0.25 * (a + h + d + rl) + 1;  // the sustain's width on the picture
      const double total = a + h + d + hold + rl;
      p.startNewSubPath(x0, yc + hh);
      double t = 0;
      auto at = [&](double ms, double level) { t += ms; p.lineTo(x0 + (float)(t / total) * w, yc + hh - (float)level * 2 * hh); };
      at(a, 1);
      at(h, 1);
      for (int i = 1; i <= 24; ++i) at(d / 24, s + (1 - s) * std::exp(-4.6 * i / 24.0));
      at(hold, s);
      for (int i = 1; i <= 24; ++i) at(rl / 24, s * std::exp(-6.9 * i / 24.0));
    } else if (lfo_ >= 0) {
      const doc::json lj = lfoJson();
      const std::string shape = lj.value("shape", "sine");
      const double phase = knob((std::string(id) + "Phase").c_str());
      if (shape == "points" && lj.contains("points") && lj["points"].is_array() && !lj["points"].empty()) {
        const doc::json& pts = lj["points"];
        for (size_t i = 0; i < pts.size(); ++i) {
          const double px = pts[i].size() > 0 ? pts[i][0].get<double>() : 0, py = pts[i].size() > 1 ? pts[i][1].get<double>() : 0;
          const juce::Point<float> pt(x0 + (float)px * w, yc - (float)py * hh);
          if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
        }
      } else {
        uint32_t seed = 0x2545F491u;
        double held = 0, lastQ = 1;
        plot([&](double x) {
          double q = x * 2 + phase; q -= std::floor(q);
          if (shape == "triangle") return 1 - 4 * std::fabs(q - 0.5);
          if (shape == "saw") return 1 - 2 * q;
          if (shape == "square") return q < 0.5 ? 1.0 : -1.0;
          if (shape == "random") {
            if (q < lastQ) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; held = (seed & 0xffff) / 32768.0 - 1; }
            lastQ = q;
            return held;
          }
          return std::sin(juce::MathConstants<double>::twoPi * q);
        });
      }
    } else if (id == "drive") {
      const double k = 1 + 6 * knob("drive");
      plot([&](double x) { const double v = std::sin(juce::MathConstants<double>::twoPi * x * 2); return std::tanh(v * k) / std::tanh(k); });
    } else if (id == "out") {
      plot([&](double x) { return 0.8 * knob("level") / 1.5 * std::sin(juce::MathConstants<double>::twoPi * x * 3); });
    } else if (id == "play") {
      const double b = knob("bend");
      plot([&](double x) { return b * (1 - std::pow(1 - x, 3)); });
    } else if (id == "delay" || id == "reverb") {
      auto tail = [&](const char* name, double def) {
        for (const auto& k : knobs_) if (k.fx == name) return k.slider->getValue();
        return def;
      };
      if (id == "delay") {  // the echoes, each quieter by the feedback
        const double fb = tail("delFeedback", 0.35);
        double level = 0.9;
        for (int e = 0; e < 8 && level > 0.02; ++e, level *= std::max(0.05, fb)) {
          const float x = x0 + w * (0.06f + 0.12f * e);
          p.startNewSubPath(x, yc + hh);
          p.lineTo(x, yc + hh - (float)level * 2 * hh);
        }
      } else {  // the reverb's tail, longer with the decay
        const double decay = tail("revDecay", 2.0);
        p.startNewSubPath(x0, yc + hh);
        for (int i = 0; i <= 120; ++i) {
          const double x = i / 120.0;
          p.lineTo(x0 + (float)x * w, yc + hh - (float)(0.9 * std::exp(-x * 6.0 / std::max(0.3, decay))) * 2 * hh);
        }
      }
    } else if (id == "macros") {
      for (int m = 0; m < 4; ++m) {
        const double v = knob(("macro" + std::to_string(m + 1)).c_str());
        const float xs = x0 + w * (m + 0.2f) / 4, xe = x0 + w * (m + 0.8f) / 4, y = yc + hh - (float)v * 2 * hh;
        p.startNewSubPath(xs, y);
        p.lineTo(xe, y);
      }
    }
  }

  WCoreModular& owner_;
  const ModuleDef& def_;
  PowerLight light_;
  int lfo_ = -1;
  bool more_ = false, hasMore_ = false;
  juce::ComboBox type_;  // the type's list (not shown: its name is drawn in the header, a click opens the list)
  bool hasType_ = false;
  std::string typeParam_;
  doc::InstrumentParamInfo typeInfo_{};
  juce::TextButton sound_;
  std::vector<Knob> knobs_;
  std::vector<Option> options_;
  std::vector<std::unique_ptr<LinkRow>> rows_;
  juce::TextButton add_;
  juce::Rectangle<int> screen_, typeArea_;
};

// ---------------------------------------------------------------- the matrix: every link of the principal, as a table
namespace {
const char* const kSources[] = {"lfo1", "lfo2", "lfo3", "lfo4", "env1", "env2", "env3", "velocity", "note",
                                "macro1", "macro2", "macro3", "macro4", "modwheel"};
constexpr int kSourceCount = 14, kRowH = 30, kOnW = 44, kSourceW = 130, kXW = 32, kPolarityW = 64, kAuxW = 130;
}  // namespace

class WCoreModular::Matrix final : public juce::Component {
public:
  explicit Matrix(WCoreModular& o) : owner_(o) {
    const doc::json links = owner_.layerSynth().value("links", doc::json::array());
    const juce::Colour c = owner_.layerColour(owner_.layer());
    for (const auto& l : links) {
      auto row = std::make_unique<Row>();
      row->id = l.value("id", "");
      const std::string id = row->id;
      // ON: the link plays, or stays kept but silent.
      row->on.colour = c;
      row->on.setToggleState(l.value("on", true), juce::dontSendNotification);
      row->on.setTooltip(u8("Liga\xc3\xa7\xc3\xa3o ligada / desligada (fica guardada)"));
      PowerLight* onp = &row->on;
      row->on.onClick = [this, id, onp] { owner_.setLink(id, {{"on", !onp->getToggleState()}}); };
      // SOURCE.
      for (int i = 0; i < kSourceCount; ++i) row->source.addItem(sourceLabel(kSources[i]), i + 1);
      for (int i = 0; i < kSourceCount; ++i) if (l.value("source", "") == kSources[i]) row->source.setSelectedId(i + 1, juce::dontSendNotification);
      juce::ComboBox* src = &row->source;
      row->source.onChange = [this, id, src] {
        const int i = src->getSelectedId() - 1;
        if (i >= 0 && i < kSourceCount) owner_.setLink(id, {{"source", kSources[i]}});
      };
      // AMOUNT: a bar from the middle, plus or minus the destination's range.
      const double range = targetRange(l.value("target", ""));
      auto& a = row->amount;
      a.setSliderStyle(juce::Slider::LinearBar);
      a.setTextBoxStyle(juce::Slider::TextBoxLeft, true, 56, kRowH - 6);
      a.setRange(-range, range, 0.0);
      a.setDoubleClickReturnValue(true, 0.0);
      a.setColour(juce::Slider::trackColourId, c.withAlpha(0.6f));
      a.setColour(juce::Slider::backgroundColourId, kInner);
      a.setColour(juce::Slider::textBoxTextColourId, kTextC);
      a.textFromValueFunction = [](double v) { return (v > 0 ? "+" : "") + juce::String(v, 2); };
      a.setValue(l.value("amount", 0.0), juce::dontSendNotification);
      juce::Slider* ap = &a;
      a.onDragStart = [this] { owner_.host_.startGesture(u8("Synth: liga\xc3\xa7\xc3\xa3o")); owner_.dragging_ = true; };
      a.onValueChange = [this, id, ap] { owner_.setLink(id, {{"amount", ap->getValue()}}); };
      a.onDragEnd = [this] { if (owner_.dragging_) owner_.host_.finishGesture(); owner_.dragging_ = false; };
      // DESTINATION.
      const auto& choices = targetChoices();
      for (size_t i = 0; i < choices.size(); ++i) row->target.addItem(choices[i].label, (int)i + 1);
      for (size_t i = 0; i < choices.size(); ++i) if (choices[i].id == l.value("target", "")) row->target.setSelectedId((int)i + 1, juce::dontSendNotification);
      juce::ComboBox* tg = &row->target;
      row->target.onChange = [this, id, tg] {
        const int i = tg->getSelectedId() - 1;
        if (i >= 0 && i < (int)targetChoices().size()) owner_.setLink(id, {{"target", targetChoices()[(size_t)i].id}});
      };
      // POLARITY: Bipolar (the source swings both ways) or Unipolar (an LFO that only opens, never closes).
      const bool uni = l.value("unipolar", false);
      row->polarity.setButtonText(uni ? u8("Uni") : u8("Bi"));
      row->polarity.setTooltip(u8("Bipolar: the source moves the value both ways. Unipolar: only one way (0 to Amount)."));
      juce::TextButton* pol = &row->polarity;
      row->polarity.onClick = [this, id, pol] { owner_.setLink(id, {{"unipolar", pol->getButtonText() == u8("Bi")}}); };
      // AUX SOURCE: scales the link by a second source (the velocity on the filter envelope); none = full.
      row->aux.addItem(u8("\xe2\x80\x94"), 1);
      for (int i = 0; i < kSourceCount; ++i) row->aux.addItem(sourceLabel(kSources[i]), i + 2);
      row->aux.setSelectedId(1, juce::dontSendNotification);
      for (int i = 0; i < kSourceCount; ++i) if (l.value("aux", "") == kSources[i]) row->aux.setSelectedId(i + 2, juce::dontSendNotification);
      row->aux.setTooltip(u8("Aux source: a second source that scales this link (none: always full)"));
      juce::ComboBox* aux = &row->aux;
      row->aux.onChange = [this, id, aux] {
        const int i = aux->getSelectedId() - 2;
        owner_.setLink(id, {{"aux", i >= 0 && i < kSourceCount ? doc::json(kSources[i]) : doc::json("")}});
      };
      // ×
      row->remove.setButtonText(u8("\xc3\x97"));
      row->remove.setTooltip(u8("Apagar esta liga\xc3\xa7\xc3\xa3o"));
      row->remove.onClick = [this, id] { owner_.removeLink(id); };
      for (juce::Component* x : std::initializer_list<juce::Component*>{&row->on, &row->source, &row->amount, &row->polarity, &row->target, &row->aux, &row->remove}) {
        x->setWantsKeyboardFocus(false);
        addAndMakeVisible(x);
      }
      rows_.push_back(std::move(row));
    }
    add_.setButtonText(u8("+ Liga\xc3\xa7\xc3\xa3o"));
    add_.setTooltip(u8("Uma liga\xc3\xa7\xc3\xa3o nova: escolhe a fonte (LFO, ENV, macro, velocidade...)."));
    add_.setWantsKeyboardFocus(false);
    add_.onClick = [this] {
      juce::PopupMenu menu;
      for (int i = 0; i < kSourceCount; ++i) menu.addItem(i + 1, sourceLabel(kSources[i]));
      juce::Component::SafePointer<Matrix> safe(this);
      menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_), [safe](int r) {
        if (safe && r > 0 && r <= kSourceCount) safe->owner_.addLink(kSources[r - 1]);
      });
    };
    addAndMakeVisible(add_);
  }
  int preferredHeight() const { return kCardHead + kRowH + (int)rows_.size() * kRowH + kSelectH + 2 * kPad; }
  void refresh(const doc::json& links) {
    for (auto& r : rows_)
      for (const auto& l : links)
        if (l.value("id", "") == r->id) {
          if (!r->amount.isMouseButtonDown()) r->amount.setValue(l.value("amount", 0.0), juce::dontSendNotification);
          r->on.setToggleState(l.value("on", true), juce::dontSendNotification);
          r->polarity.setButtonText(l.value("unipolar", false) ? u8("Uni") : u8("Bi"));
          int aux = 1;
          for (int i = 0; i < kSourceCount; ++i) if (l.value("aux", "") == kSources[i]) aux = i + 2;
          r->aux.setSelectedId(aux, juce::dontSendNotification);
        }
  }
  void paint(juce::Graphics& g) override {
    const juce::Colour c = owner_.layerColour(owner_.layer());
    theme::paintCard(g, getLocalBounds().toFloat(), (float)kCardHead, c, 1.0f, false);
    g.setColour(kTextC);
    g.setFont(font(theme::kFontBody, true));
    g.drawText(u8("MODULATION MATRIX \xc2\xb7 ") + juce::String((int)rows_.size()),
               getLocalBounds().removeFromTop(kCardHead).withTrimmedLeft(12), juce::Justification::centredLeft);
    // The columns' names.
    auto h = columns(getLocalBounds().reduced(kPad, 0).withTrimmedTop(kCardHead).removeFromTop(kRowH));
    g.setColour(kMutedC);
    g.setFont(font(theme::kFontSmall, true));
    const char* names[kCols] = {"ON", "SOURCE", "AMOUNT", "POLARITY", "DESTINATION", "AUX SOURCE", ""};
    for (int i = 0; i < kCols; ++i) g.drawText(u8(names[i]), h[(size_t)i], i == 0 ? juce::Justification::centred : juce::Justification::centredLeft);
    g.setColour(kLine);
    for (size_t i = 0; i <= rows_.size(); ++i)
      g.drawHorizontalLine(kCardHead + kRowH * (int)(i + 1), (float)kPad, (float)(getWidth() - kPad));
    if (rows_.empty()) {
      g.setColour(kDimC);
      g.setFont(font(theme::kFontSmall));
      g.drawText(u8("Ainda sem liga\xc3\xa7\xc3\xb5" "es."), juce::Rectangle<int>(kPad + kOnW, kCardHead + kRowH, 300, kRowH), juce::Justification::centredLeft);
    }
  }
  void resized() override {
    auto b = getLocalBounds().reduced(kPad, 0);
    b.removeFromTop(kCardHead + kRowH);
    for (auto& r : rows_) {
      auto col = columns(b.removeFromTop(kRowH));
      r->on.setBounds(col[0]);
      r->source.setBounds(col[1].reduced(0, 3));
      r->amount.setBounds(col[2].reduced(0, 3));
      r->polarity.setBounds(col[3].reduced(0, 3));
      r->target.setBounds(col[4].reduced(0, 3));
      r->aux.setBounds(col[5].reduced(0, 3));
      r->remove.setBounds(col[6].reduced(4, 3));
    }
    if (rows_.empty()) b.removeFromTop(kRowH);
    b.removeFromTop(kPad / 2);
    add_.setBounds(b.removeFromTop(kSelectH).removeFromLeft(130));
  }

private:
  // One link per line (the brief): ON | SOURCE | AMOUNT | POLARITY | DESTINATION | AUX SOURCE | ×. Curve, Invert and
  // Output are not in the engine's links yet (wave_mod.hpp), so they are not shown.
  static constexpr int kCols = 7;
  static std::array<juce::Rectangle<int>, kCols> columns(juce::Rectangle<int> line) {
    std::array<juce::Rectangle<int>, kCols> out;
    out[0] = line.removeFromLeft(kOnW);
    out[6] = line.removeFromRight(kXW);
    out[1] = line.removeFromLeft(kSourceW);
    line.removeFromLeft(8);
    out[5] = line.removeFromRight(kAuxW);
    line.removeFromRight(8);
    out[2] = line.removeFromLeft(std::max(90, (line.getWidth() - kPolarityW - 16) / 2));
    line.removeFromLeft(8);
    out[3] = line.removeFromLeft(kPolarityW);
    line.removeFromLeft(8);
    out[4] = line;  // the destination: the widest
    return out;
  }
  struct Row {
    std::string id;
    PowerLight on;
    juce::ComboBox source, target, aux;
    juce::Slider amount;
    juce::TextButton polarity, remove;
  };
  WCoreModular& owner_;
  std::vector<std::unique_ptr<Row>> rows_;
  juce::TextButton add_;
};

// ---------------------------------------------------------------- the header: the name, A/B, tabs, waves, preset
class WCoreModular::Header final : public juce::Component {
public:
  explicit Header(WCoreModular& o) : owner_(o), ab_(o.host_) {
    // A/B: two versions of the track's sound, as on a VST plugin (the same A/B as the track's other panels).
    ab_.setTrack(owner_.trackId_);
    addAndMakeVisible(ab_);
    for (int i = 0; i < 2; ++i) {
      auto& b = tabs_[i];
      b.setClickingTogglesState(false);
      b.onClick = [this, i] { owner_.showTab(i); };
    }
    tabs_[0].setButtonText("OSC");
    // Panels: the other Core windows, from here (the design's Panels list, on the tab row).
    panels_.setButtonText(u8("Panels  \xe2\x96\xbe"));
    panels_.setTooltip(u8("Abrir outro painel do Core para esta pista"));
    panels_.onClick = [this] {
      juce::PopupMenu menu;
      const auto& all = doc::corePlugins();
      for (size_t i = 0; i < all.size(); ++i)
        if (all[i].id != "wcore") menu.addItem((int)i + 1, u8(all[i].fullName));
      juce::Component::SafePointer<Header> safe(this);
      menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&panels_), [safe](int r) {
        const auto& list = doc::corePlugins();
        if (safe && r > 0 && r <= (int)list.size() && safe->owner_.onOpenPanel) safe->owner_.onOpenPanel(safe->owner_.trackId_, list[(size_t)r - 1].id);
      });
    };
    for (int i = 0; i < doc::kWaveLayers; ++i) {
      principals_[i].onClick = [this, i] { owner_.selectPrincipal(i); };
      principals_[i].onMenu = [this, i] { owner_.showWaveMenu(i, principals_[i]); };
    }
    // The preset chip beside the waves: ‹ name ›; the name opens the list, the chevrons step through it.
    prev_.getProperties().set(theme::kIconProperty, "left");
    next_.getProperties().set(theme::kIconProperty, "right");
    prev_.setTooltip(u8("Preset anterior"));
    next_.setTooltip(u8("Preset seguinte"));
    prev_.onClick = [this] { step(-1); };
    next_.onClick = [this] { step(1); };
    preset_.setTooltip(u8("Os presets do synth, por tipo"));
    preset_.onClick = [this] {
      juce::PopupMenu menu;
      const auto& all = doc::wavePresets();
      for (const auto& [cat, catName] : doc::waveCategories()) {
        juce::PopupMenu sub;
        for (size_t i = 0; i < all.size(); ++i)
          if (cat == all[i].category) sub.addItem((int)i + 1, u8(all[i].name) + u8("   \xc2\xb7 ") + u8(all[i].style), true, (int)i == chosen_);
        menu.addSubMenu(u8(catName), sub);
      }
      juce::Component::SafePointer<Header> safe(this);
      menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&preset_), [safe](int r) {
        if (safe && r > 0) safe->choose(r - 1);
      });
    };
    track_.setButtonText(u8("Pista"));
    track_.setTooltip(u8("A modula\xc3\xa7\xc3\xa3o da pista (ducking e curvas), a mesma do Mixer."));
    track_.onClick = [this] { if (owner_.onOpenModulation) owner_.onOpenModulation(owner_.trackId_); };
    for (juce::Component* x : std::initializer_list<juce::Component*>{&prev_, &preset_, &next_, &track_, &panels_, &tabs_[0], &tabs_[1]}) {
      x->setWantsKeyboardFocus(false);
      addAndMakeVisible(x);
    }
    for (auto& b : principals_) addAndMakeVisible(b);
    refresh();
  }
  void refresh() {
    ab_.documentChanged();
    const doc::Track* t = owner_.track();
    // Text-only buttons: dark when off, light when on (the theme inverts them on hover); never the highlight blue.
    for (int i = 0; i < 2; ++i) {
      tabs_[i].setColour(juce::TextButton::buttonColourId, theme::kControl);
      tabs_[i].setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
      tabs_[i].setToggleState(i == owner_.tab_, juce::dontSendNotification);
    }
    const int links = (int)owner_.layerSynth().value("links", doc::json::array()).size();
    tabs_[1].setButtonText(links > 0 ? u8("MATRIX \xc2\xb7 ") + juce::String(links) : u8("MATRIX"));
    for (int i = 0; i < doc::kWaveLayers; ++i) {
      auto& b = principals_[i];
      const bool exists = t && doc::waveLayerExists(*t, i);
      b.exists = exists;
      b.dot = owner_.layerColour(i);
      b.setButtonText(exists ? u8("Wave ") + juce::String(i + 1) : u8("+ Wave"));
      b.setTooltip(exists ? u8("Wave ") + juce::String(i + 1) + u8(": o seu pr\xc3\xb3prio conjunto de m\xc3\xb3" "dulos")
                          : u8("Uma Wave nova (uma c\xc3\xb3pia da primeira)"));
      b.setVisible(exists || (i > 0 && t && doc::waveLayerExists(*t, i - 1)));
      b.setToggleState(i == owner_.layer(), juce::dontSendNotification);
      b.repaint();
    }
    const auto& all = doc::wavePresets();
    preset_.setButtonText(chosen_ >= 0 && chosen_ < (int)all.size() ? u8(all[(size_t)chosen_].category).toUpperCase().substring(0, 1) +
                                                                          u8(all[(size_t)chosen_].category).substring(1) + u8(" \xc2\xb7 ") + u8(all[(size_t)chosen_].name)
                                                                    : u8("Presets"));
    resized();
    repaint();
  }
  void paint(juce::Graphics& g) override {
    g.fillAll(kBg);
    // The title band: CORE · W-CORE SYNTH, spaced capitals, as in the design.
    auto band = getLocalBounds().removeFromTop(kRow1);
    g.setColour(kPanelC);
    g.fillRect(band);
    g.setColour(kLine);
    g.drawHorizontalLine(band.getBottom() - 1, 0.0f, (float)getWidth());
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());
    g.setColour(theme::kTitle);
    g.setFont(font(theme::kFontHeading, true).withExtraKerningFactor(0.12f));
    g.drawText(u8("CORE \xc2\xb7 W-CORE SYNTH"), band.withTrimmedLeft(16), juce::Justification::centredLeft);
  }
  void resized() override {
    auto b = getLocalBounds();
    auto row1 = b.removeFromTop(kRow1).reduced(10, 8);
    ab_.setBounds(row1.removeFromRight(AbSwitch::kWidth));
    row1.removeFromRight(8);
    track_.setBounds(row1.removeFromRight(64));
    auto row2 = b.removeFromTop(kRow2).reduced(10, 5);
    tabs_[0].setBounds(row2.removeFromLeft(64));
    row2.removeFromLeft(4);
    tabs_[1].setBounds(row2.removeFromLeft(96));
    row2.removeFromLeft(4);
    panels_.setBounds(row2.removeFromLeft(100));
    auto row3 = b.reduced(10, 5);
    for (auto& p : principals_) {
      if (!p.isVisible()) continue;
      p.setBounds(row3.removeFromLeft(88));
      row3.removeFromLeft(4);
    }
    row3.removeFromLeft(8);
    prev_.setBounds(row3.removeFromLeft(28));
    row3.removeFromLeft(2);
    const juce::Font f{juce::FontOptions(theme::kFontStrong)};
    const int w = juce::GlyphArrangement::getStringWidthInt(f, preset_.getButtonText()) + 28;
    preset_.setBounds(row3.removeFromLeft(std::min(std::max(110, w), std::max(110, row3.getWidth() - 30))));
    row3.removeFromLeft(2);
    next_.setBounds(row3.removeFromLeft(28));
  }

private:
  static constexpr int kRow1 = 46, kRow2 = 36;
  void choose(int index) {
    const auto& all = doc::wavePresets();
    if (index < 0 || index >= (int)all.size()) return;
    chosen_ = index;
    owner_.applyPreset(all[(size_t)index].id);
    refresh();
  }
  void step(int by) {
    const int n = (int)doc::wavePresets().size();
    if (n == 0) return;
    choose(chosen_ < 0 ? (by > 0 ? 0 : n - 1) : (chosen_ + by + n) % n);
  }
  WCoreModular& owner_;
  AbSwitch ab_;
  juce::TextButton tabs_[2];
  WaveChip principals_[doc::kWaveLayers];
  juce::TextButton prev_, preset_, next_, track_, panels_;
  int chosen_ = -1;  // the preset last chosen in this window (the document keeps the sound, not its name)
};

// ---------------------------------------------------------------- WCoreModular
WCoreModular::WCoreModular(PatternEditor::Host& host, std::string trackId) : host_(host), trackId_(std::move(trackId)) {
  header_ = std::make_unique<Header>(*this);
  addAndMakeVisible(*header_);
  content_ = std::make_unique<Content>(*this);
  view_.setViewedComponent(content_.get(), false);
  view_.setScrollBarsShown(true, true);
  addAndMakeVisible(view_);
  rebuild();
  setSize(1240, 860);
}

WCoreModular::~WCoreModular() { if (dragging_) host_.finishGesture(); }

const doc::Track* WCoreModular::track() const {
  const doc::Track* t = host_.currentDocument().findTrack(trackId_);
  return t && t->instrument.kind == "builtin" && t->instrument.name == doc::kWaveInstrument ? t : nullptr;
}

doc::json WCoreModular::layerSynth() const {
  const doc::Track* t = track();
  if (!t) return doc::json::object();
  const doc::json j = doc::waveSynthLayer(*t, layer_);
  return j.is_object() ? j : doc::json::object();
}

std::string WCoreModular::structureKey() const {
  const doc::Track* t = track();
  std::string key = std::to_string(layer_) + "T" + std::to_string(tab_) + editing_ + "|";
  for (const auto& m : more_) key += m + ",";
  for (int z : foldedZones_) key += "z" + std::to_string(z);
  if (!t) return key + "none";
  for (int i = 0; i < doc::kWaveLayers; ++i) key += doc::waveLayerExists(*t, i) ? "1" : "0";
  if (const doc::SampleRef* z = doc::waveLayerSound(*t, layer_)) key += "|snd:" + z->name;
  if (t->extra.is_object() && t->extra.contains("waveColours")) key += "|col:" + t->extra["waveColours"].dump();
  key += "|tail" + std::to_string(tailIndex(*t));
  const doc::json s = layerSynth();
  for (const auto& l : s.value("links", doc::json::array()))
    key += "|" + l.value("id", "") + ":" + l.value("source", "") + ">" + l.value("target", "");
  for (const auto& l : s.value("lfos", doc::json::array()))
    key += "|" + l.value("shape", "") + l.value("mode", "") + (l.value("sync", true) ? "s" : "h") + (l.contains("points") ? l["points"].dump() : "");
  return key;
}

void WCoreModular::rebuild() {
  identifyWithTrack(*this, host_.currentDocument(), trackId_);  // knobs, curves and lights in the track's colour
  builtFor_ = structureKey();
  content_->parts.clear();
  content_->zones.clear();
  content_->removeAllChildren();
  const int width = std::max(700, view_.getMaximumVisibleWidth());
  int y = kPad;
  auto place = [&](std::unique_ptr<juce::Component> c) {
    content_->addAndMakeVisible(*c);
    content_->parts.push_back(std::move(c));
  };
  if (tab_ == 1) {  // MATRIX: the table of every link, the width of the window
    auto m = std::make_unique<Matrix>(*this);
    m->setBounds(kPad, y, width - 2 * kPad, m->preferredHeight());
    y += m->getHeight() + kPad;
    place(std::move(m));
    content_->setSize(width, y);
    header_->refresh();
    content_->repaint();
    return;
  }
  // OSC: one row per zone, its name vertical on the left; the modules of a row fill its width in the brief's
  // proportions and share its height. Rows and cards are contiguous, one thin line between them.
  const int left = kPad + kZoneCol - 1, avail = width - left - kPad;
  int right = width - kPad;
  for (int z = 0; z < kZones; ++z) {
    std::vector<const ModuleDef*> row;
    juce::String names;
    const doc::Track* tr = track();
    for (const auto& m : modules()) {
      if (m.zone != z || (isTailModule(m.id) && !(tr && tailIndex(*tr) >= 0))) continue;
      if (std::string(m.id) == "lfo4" && !lfoUsed(3)) continue;  // the brief's three LFOs; the fourth when it is linked
      row.push_back(&m);
      names << (names.isEmpty() ? juce::String() : u8(" \xc2\xb7 ")) << u8(m.name);
    }
    if (row.empty()) continue;
    if (foldedZones_.count(z)) {
      content_->zones.push_back({z, juce::Rectangle<int>(kPad, y, width - 2 * kPad, kZoneBar), true, names});
      y += kZoneBar + 4;
      continue;
    }
    std::vector<std::unique_ptr<Card>> made;
    std::vector<double> weight;
    for (const ModuleDef* m : row) {
      made.push_back(std::make_unique<Card>(*this, *m));
      const std::string id = m->id;
      weight.push_back(id == "osc1" ? 1.2 : id == "osc2" ? 1.1 : id == "sub" || id == "noise" ? 0.78 : id == "grain" ? 1.05 : id == "out" ? 0.5 : 1.0);
    }
    // Lines of cards: all on one line when they fit; else (a narrow window) the brief's narrow layout, two then the rest.
    int need = 0;
    for (auto& c : made) need += c->minimumWidth();
    std::vector<std::pair<size_t, size_t>> lines;
    if (need <= avail || made.size() < 4) lines.push_back({0, made.size()});
    else { lines.push_back({0, 2}); lines.push_back({2, made.size()}); }
    const int top = y;
    for (auto [i, end] : lines) {
      double sum = 0;
      for (size_t k = i; k < end; ++k) sum += lines.size() > 1 ? 1.0 : weight[k];
      int height = 0;
      std::vector<int> xs;
      double acc = 0;
      xs.push_back(left);
      for (size_t k = i; k < end; ++k) {
        acc += lines.size() > 1 ? 1.0 : weight[k];
        xs.push_back(left + juce::roundToInt(avail * acc / sum));
      }
      for (size_t k = i; k < end; ++k) height = std::max(height, made[k]->preferredHeight(xs[k - i + 1] - xs[k - i] + 1));
      for (size_t k = i; k < end; ++k) {
        made[k]->setBounds(xs[k - i], y, xs[k - i + 1] - xs[k - i] + 1, height);  // +1: the neighbours share a line
        place(std::move(made[k]));
      }
      y += height - 1;
    }
    y += 1;
    content_->zones.push_back({z, juce::Rectangle<int>(kPad, top, kZoneCol, y - top), false, {}});
    y += 4;  // a little room between the rows' frames
  }
  y += 1;
  // The minimised bars and the zone names end where the modules end, not at the window's edge.
  for (auto& z : content_->zones) if (z.folded) z.label.setRight(right);
  content_->setSize(std::max(width, right + kPad), y + kPad);
  header_->refresh();
  content_->repaint();
}

bool WCoreModular::lfoUsed(int lfo) const {
  const std::string id = "lfo" + std::to_string(lfo + 1);
  for (const auto& l : layerSynth().value("links", doc::json::array()))
    if (l.value("source", "") == id) return true;
  return false;
}

void WCoreModular::documentChanged() {
  if (!track()) return;
  identifyWithTrack(*this, host_.currentDocument(), trackId_);
  if (structureKey() != builtFor_) { rebuild(); return; }
  header_->refresh();
  const doc::json links = layerSynth().value("links", doc::json::array());
  for (auto& p : content_->parts) {
    if (auto* c = dynamic_cast<Card*>(p.get())) c->refresh();
    else if (auto* m = dynamic_cast<Matrix*>(p.get())) m->refresh(links);
  }
}

void WCoreModular::paint(juce::Graphics& g) { g.fillAll(kBg); }

void WCoreModular::resized() {
  auto b = getLocalBounds();
  header_->setBounds(b.removeFromTop(kHeaderH));
  view_.setBounds(b);
  rebuild();  // the rows wrap to the width
}

void WCoreModular::setTail(const std::string& param, double v) {
  const doc::Track* t = track();
  const int at = t ? tailIndex(*t) : -1;
  if (at < 0) return;
  const doc::EffectParamInfo* p = tailParam(param);
  v = p ? std::clamp(v, p->min, p->max) : std::clamp(v, 0.0, 1.0);
  if (std::fabs(tailValue(*t, param, p ? p->def : -1) - v) < 1e-9) return;
  const std::string target = trackId_;
  host_.applyEdit([&](doc::Document& d) { doc::setEffectParam(d, target, at, param, v); },
                  u8("Synth ") + (param == "dry" ? u8("Dry") : param == "amount" ? u8("Wash") : p ? u8(p->label) : u8(param)));
}

void WCoreModular::setParam(const std::string& param, double v) {
  const doc::Track* t = track();
  if (!t) return;
  const doc::InstrumentParamInfo* p = doc::findInstrumentParam(*t, param);
  if (!p) return;
  v = std::clamp(v, p->min, p->max);
  if (std::fabs(doc::instrumentParamValue(*t, *p) - v) < 1e-9) return;
  const std::string target = trackId_;
  host_.applyEdit([&](doc::Document& d) { doc::setInstrumentParam(d, target, param, v); }, u8("Synth ") + u8(p->label));
}

// A Wave's colour: its own when one was chosen (the track's "waveColours"), else a shade of the track's colour.
// The synth's own scale (Diogo 2026-11-09 "um crescendo de cores iguais, palette de azul e verde diferente dos
// instrumentos, mais transparente, começa osc claro e vai escurecendo cada row"): light frost blue on the sources, a
// little deeper on each row down to Global (the UI colour list, CORE-MANUAL app/ui-colours.md). Headers at 35 %, a 2 px
// frame in the colour, the body charcoal; the Waves' Frosted Blue stays on the curves, arcs and dots.
juce::Colour WCoreModular::rowColour(int zone) const {
  if (const doc::Track* t = track(); t && t->extra.is_object() && t->extra.contains("waveColours")) {
    const doc::json& c = t->extra["waveColours"];
    if (c.is_array() && layer_ < (int)c.size() && c[(size_t)layer_].is_string() && c[(size_t)layer_].get<std::string>().size() == 7)
      return layerColour(layer_);
  }
  static const juce::uint32 scale[kZones] = {0xff9ef5e0, 0xff6ee6d6, 0xff3fcfcb, 0xff2bb3c2, 0xff2496b3, 0xff237ca3};  // ui-colours.md (even steps, ending in petrol blue)
  return juce::Colour(scale[std::clamp(zone, 0, kZones - 1)]);
}

// The four Waves' own colours (ui-colours.md): Frosted Blue, spaced so each Wave is told apart; never a track colour.
struct WaveTone { const char* name; juce::uint32 argb; const char* hex; };
constexpr WaveTone kWaveFrost[4] = {{"Frost", 0xff78eef5, "#78eef5"}, {"Ice", 0xffb8ffff, "#b8ffff"},
                                    {"Snow", 0xffe3fffc, "#e3fffc"}, {"Glacier", 0xff4fd8e8, "#4fd8e8"}};

juce::Colour WCoreModular::layerColour(int layer) const {
  if (const doc::Track* t = track(); t && t->extra.is_object() && t->extra.contains("waveColours")) {
    const doc::json& c = t->extra["waveColours"];
    if (c.is_array() && layer < (int)c.size() && c[(size_t)layer].is_string())
      for (const auto& w : kWaveFrost)  // only a Wave colour counts (a track colour chosen before 2026-11-09 does not)
        if (juce::String(c[(size_t)layer].get<std::string>()).equalsIgnoreCase(w.hex)) return juce::Colour(w.argb);
  }
  // The highlights (curves, knob arcs, dots) in the brief's Frosted Blue, one tone per Wave (Diogo 2026-11-09 "realçar
  // numa cor mais viva e light como azul frost"); a colour chosen on the Wave's right-click wins.
  return juce::Colour(kWaveFrost[std::clamp(layer, 0, 3)].argb);
}

// Right-click on a Wave (Diogo 2026-11-09: "muda cor ou fecha wave"): its colour, or remove it (Wave 1 stays).
void WCoreModular::showWaveMenu(int layer, juce::Component& at) {
  juce::PopupMenu menu, colour;
  menu.addSectionHeader(u8("Wave ") + juce::String(layer + 1));
  colour.addItem(1, u8("Automatic (from the track)"));
  colour.addSeparator();
  for (int i = 0; i < (int)std::size(kWaveFrost); ++i)  // only the Waves' colours: a Wave never takes a track's colour
    colour.addItem(juce::PopupMenu::Item(u8(kWaveFrost[i].name)).setID(10 + i).setColour(juce::Colour(kWaveFrost[i].argb)));
  menu.addSubMenu(u8("Colour"), colour);
  menu.addSeparator();
  menu.addItem(juce::PopupMenu::Item(u8("Remove Wave ") + juce::String(layer + 1)).setID(2).setEnabled(layer > 0));
  juce::Component::SafePointer<WCoreModular> safe(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe, layer](int r) {
    if (!safe || r <= 0) return;
    const std::string target = safe->trackId_;
    if (r == 2) {
      safe->host_.applyEdit([&](doc::Document& d) {
        doc::removeWaveLayer(d, target, layer);
        if (doc::Track* t = &doc::trackById(d, target); t->extra.is_object() && t->extra.contains("waveColours") && t->extra["waveColours"].is_array()
            && layer < (int)t->extra["waveColours"].size())
          t->extra["waveColours"][(size_t)layer] = nullptr;
      }, u8("Synth: remove wave"));
      if (safe->layer_ == layer) safe->layer_ = 0;
      safe->rebuild();
      return;
    }
    std::string hex;
    if (r >= 10 && r < 10 + (int)std::size(kWaveFrost)) hex = kWaveFrost[r - 10].hex;
    safe->host_.applyEdit([&](doc::Document& d) {
      doc::Track* t = &doc::trackById(d, target);
      if (!t->extra.is_object()) t->extra = doc::json::object();
      doc::json cols = t->extra.value("waveColours", doc::json::array());
      if (!cols.is_array()) cols = doc::json::array();
      while ((int)cols.size() < doc::kWaveLayers) cols.push_back(nullptr);
      cols[(size_t)layer] = hex.empty() ? doc::json(nullptr) : doc::json(hex);
      bool any = false;
      for (const auto& c : cols) any = any || c.is_string();
      if (any) t->extra["waveColours"] = cols; else t->extra.erase("waveColours");
    }, u8("Synth: wave colour"));
    safe->rebuild();
  });
}

void WCoreModular::selectPrincipal(int layer) {
  const doc::Track* t = track();
  if (!t || layer < 0 || layer >= doc::kWaveLayers) return;
  if (!doc::waveLayerExists(*t, layer)) {
    const std::string target = trackId_;
    int made = -1;
    host_.applyEdit([&](doc::Document& d) { made = doc::addWaveLayer(d, target); }, u8("Synth: + wave"));
    if (made > 0) layer = made;
    else return;
  }
  layer_ = layer;
  rebuild();
}

void WCoreModular::edit(const std::string& module) {
  editing_ = module;
  if (!module.empty()) {
    tab_ = 0;
    for (const auto& m : modules()) if (module == m.id) foldedZones_.erase(m.zone);
  }
  rebuild();
}

void WCoreModular::showTab(int tab) {
  tab_ = std::clamp(tab, 0, 1);
  rebuild();
}

void WCoreModular::showMatrix(bool show) { showTab(show ? 1 : 0); }

void WCoreModular::foldZone(int zone, bool folded) {
  if (folded) foldedZones_.insert(zone); else foldedZones_.erase(zone);
  rebuild();
}

void WCoreModular::showMore(const std::string& module, bool show) {
  if (show) more_.insert(module); else more_.erase(module);
  rebuild();
}

void WCoreModular::addLink(const std::string& source) {
  if (!track()) return;
  const std::string target = trackId_;
  const int l = layer_;
  try {
    host_.applyEdit([&](doc::Document& d) { doc::addWaveLink(d, target, l, source, "tone", 0.0); },
                    u8("Synth: liga\xc3\xa7\xc3\xa3o de ") + sourceLabel(source));
  } catch (const std::exception&) {}  // 32 links at most: nothing happens
}

void WCoreModular::setLink(const std::string& linkId, const doc::json& changes) {
  if (!track()) return;
  const std::string target = trackId_;
  const int l = layer_;
  try {
    host_.applyEdit([&](doc::Document& d) { doc::setWaveLink(d, target, l, linkId, changes); }, u8("Synth: liga\xc3\xa7\xc3\xa3o"));
  } catch (const std::exception&) {}
}

void WCoreModular::removeLink(const std::string& linkId) {
  if (!track()) return;
  const std::string target = trackId_;
  const int l = layer_;
  try {
    host_.applyEdit([&](doc::Document& d) { doc::removeWaveLink(d, target, l, linkId); }, u8("Synth: apagar liga\xc3\xa7\xc3\xa3o"));
  } catch (const std::exception&) {}
}

void WCoreModular::setLfo(int lfo, const doc::json& changes) {
  if (!track()) return;
  const std::string target = trackId_;
  const int l = layer_;
  try {
    host_.applyEdit([&](doc::Document& d) { doc::setWaveLfo(d, target, l, lfo, changes); }, u8("Synth: LFO ") + juce::String(lfo + 1));
  } catch (const std::exception&) {}
}

void WCoreModular::applyPreset(const std::string& presetId) {
  const doc::WavePreset* p = doc::findWavePreset(presetId);
  if (!p || !track()) return;
  const std::string target = trackId_;
  host_.applyEdit([&](doc::Document& d) { doc::setWave(d, target, presetId); }, u8("Synth: ") + u8(p->name));
}

void WCoreModular::chooseGrainSound(juce::Component* from) {
  const doc::SampleIndex* index = sampleLibrary ? sampleLibrary() : nullptr;
  juce::PopupMenu menu;
  menu.addItem(1, u8("Wave (a onda do synth)"));
  menu.addSeparator();
  std::map<std::string, juce::PopupMenu> byRole;
  std::map<std::string, int> counts;
  constexpr int kPerRole = 80;  // a menu stays readable
  if (index)
    for (size_t i = 0; i < index->samples.size(); ++i) {
      const doc::LibrarySample& s = index->samples[i];
      const std::string role = s.info.role.empty() ? std::string("other") : s.info.role;
      if (++counts[role] > kPerRole) continue;
      byRole[role].addItem((int)i + 2, u8(s.displayName.empty() ? s.name : s.displayName));
    }
  if (byRole.empty()) menu.addItem(juce::PopupMenu::Item(u8("A biblioteca de samples ainda n\xc3\xa3o foi lida (Library)")).setEnabled(false));
  for (auto& [role, sub] : byRole) menu.addSubMenu(u8(role), sub);
  juce::Component::SafePointer<WCoreModular> safe(this);
  const int l = layer_;
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(from), [safe, index, l](int chosen) {
    if (!safe || chosen < 1) return;
    WCoreModular& self = *safe;
    const doc::SampleRef* zone = nullptr;
    doc::SampleRef picked;
    if (chosen >= 2) {
      if (!index || chosen - 2 >= (int)index->samples.size()) return;
      picked = doc::sampleRef(index->samples[(size_t)chosen - 2]);
      zone = &picked;
    }
    if (!self.track()) return;
    const std::string target = self.trackId_;
    self.host_.applyEdit([&](doc::Document& d) { doc::setWaveLayerSound(d, target, l, zone); },
                         u8("Sample do synth: ") + (zone ? u8(zone->name) : u8("wave")));
    // A sound picked with the Sample module at 0 would change nothing: bring the grains in, so the choice is heard.
    if (zone) {
      const doc::Track* t = self.track();
      const doc::InstrumentParamInfo* p = t ? doc::findInstrumentParam(*t, doc::waveLayerParam(l, "grain")) : nullptr;
      if (p && doc::instrumentParamValue(*t, *p) <= 0) self.setParam(doc::waveLayerParam(l, "grain"), 0.7);
    }
  });
}

// ---------------------------------------------------------------- the window
WCoreWindow::WCoreWindow(PatternEditor::Host& host, const std::string& trackId, const juce::String& trackName)
    : juce::DocumentWindow(u8("W-Core \xc2\xb7 ") + trackName, kBg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton) {
  auto modular = std::make_unique<WCoreModular>(host, trackId);
  modular_ = modular.get();
  setUsingNativeTitleBar(true);
  setContentOwned(modular.release(), true);
  setResizable(true, false);
  setResizeLimits(720, 480, 4000, 3000);
  centreWithSize(1240, 860);
}

}  // namespace core::app
