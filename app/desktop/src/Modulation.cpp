#include "Modulation.h"
#include "TrackColour.h"
#include "Language.h"
#include "fcore.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include "Theme.h"
#include "edit.hpp"
#include "ModulationLive.h"

namespace core::app {
using namespace theme;
using nlohmann::json;

namespace {
juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }
juce::String u8(const char* t) { return juce::String::fromUTF8(t); }

constexpr int kRow = 24;
constexpr size_t kPerTab = 8;  // per tab; the engine's limit (16) is for all of a track's modulators

// The LFO shapes as the engine names them, and as the window shows them.
const std::vector<std::pair<std::string, const char*>>& shapes() {
  static const std::vector<std::pair<std::string, const char*>> s = {
      {"sine", "Sine"}, {"triangle", "Triangle"}, {"square", "Square"}, {"saw", "Saw (down)"}, {"ramp", "Ramp (up)"}, {"random", "Random (held)"}};
  return s;
}
// Rates in note lengths (beats per cycle; for steps, per step).
const std::vector<std::pair<double, const char*>>& rates() {
  static const std::vector<std::pair<double, const char*>> r = {
      {0.125, "1/32"}, {0.25, "1/16"}, {0.5, "1/8"}, {1, "1/4"}, {2, "1/2"}, {4, "1 bar"}, {8, "2 bars"}, {16, "4 bars"}, {32, "8 bars"}, {64, "16 bars"}};
  return r;
}
const std::vector<int>& stepCounts() {
  static const std::vector<int> c = {4, 8, 12, 16};
  return c;
}

// Which tab a modulator belongs to. FCore's own movements (on its card) and the Mixer's ducking ("duck",
// set from the strip) are not in any tab; neither is a "rise" (FCore only).
std::optional<ModulationPanel::Mode> modeOf(const json& m) {
  if (doc::isFCoreModulator(m) || m.value("id", "") == "duck") return std::nullopt;
  const std::string s = m.value("shape", "sine");
  if (s == "envelope") return ModulationPanel::Mode::Env;
  if (s == "steps") return ModulationPanel::Mode::Step;
  for (const auto& sh : shapes()) if (sh.first == s) return ModulationPanel::Mode::Lfo;
  return std::nullopt;
}
const char* prefix(ModulationPanel::Mode m) { return m == ModulationPanel::Mode::Env ? "ENV" : m == ModulationPanel::Mode::Step ? "STEP" : "LFO"; }

std::vector<double> stepsOf(const json& m) {
  std::vector<double> v;
  if (m.contains("steps") && m["steps"].is_array())
    for (const auto& s : m["steps"]) if (s.is_number()) v.push_back(s.get<double>());
  return v;
}

using live::lfoAt;
using live::envAt;

juce::String number(double v, const juce::String& unit) {
  const double a = std::fabs(v);
  if (unit == "Hz" && a >= 1000) return juce::String(v / 1000, 2) + " kHz";
  const juce::String n = a >= 100 ? juce::String(juce::roundToInt(v)) : juce::String(v, a >= 10 ? 1 : 2);
  return n + (unit.isEmpty() ? juce::String() : " " + unit);
}
juce::String ms(double v) { return v >= 1000 ? juce::String(v / 1000, 2) + " s" : juce::String(juce::roundToInt(v)) + " ms"; }
}  // namespace

ModulationPanel::ModulationPanel(PatternEditor::Host& host) : host_(host) {
  title_.setText("MODULATION", juce::dontSendNotification);
  title_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
  title_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(title_);
  lfoTab_.setButtonText("LFO");
  envTab_.setButtonText("ENV");
  macroTab_.setButtonText("MACRO");
  stepTab_.setButtonText("STEP");
  lfoTab_.setTooltip(u8("LFO: a wave that moves a parameter up and down, in time with the song"));
  envTab_.setTooltip(u8("ENV: a parameter jumps at every note (or every kick) and falls back"));
  stepTab_.setTooltip(u8("STEP: a value for every step, drawn with the mouse, in time with the song"));
  macroTab_.setEnabled(false);
  macroTab_.setTooltip(u8("Not available yet: one knob that moves several parameters at once"));
  for (auto* b : {&lfoTab_, &envTab_, &stepTab_}) b->setColour(juce::TextButton::buttonOnColourId, kPrimary);
  lfoTab_.onClick = [this] { setMode(Mode::Lfo); };
  envTab_.onClick = [this] { setMode(Mode::Env); };
  stepTab_.onClick = [this] { setMode(Mode::Step); };
  lfoTab_.setToggleState(true, juce::dontSendNotification);
  remove_.setButtonText("Remove");
  remove_.onClick = [this] {
    const std::string id = chosen_;
    host_.applyEdit([&](doc::Document& d) { doc::removeModulator(d, trackId_, id); }, u8("Remove ") + prefix(mode_));
  };
  for (auto* b : {&lfoTab_, &envTab_, &macroTab_, &stepTab_, &remove_}) {
    b->setWantsKeyboardFocus(false);
    addAndMakeVisible(b);
  }
  empty_.setJustificationType(juce::Justification::centred);
  empty_.setColour(juce::Label::textColourId, kMuted);
  addAndMakeVisible(empty_);
  name_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  name_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(name_);

  for (auto* l : {&shapeLabel_, &rateLabel_, &depthLabel_, &destinationLabel_, &phaseLabel_, &rangeLabel_, &sourceLabel_, &attackLabel_, &decayLabel_,
                  &stepCountLabel_}) {
    l->setFont(juce::FontOptions(theme::kFontSmall));
    l->setBorderSize({});
    l->setColour(juce::Label::textColourId, kMuted);
    addAndMakeVisible(*l);
  }
  shapeLabel_.setText("Shape", juce::dontSendNotification);
  depthLabel_.setText("Depth", juce::dontSendNotification);
  destinationLabel_.setText("Destination", juce::dontSendNotification);
  phaseLabel_.setText("Phase", juce::dontSendNotification);
  sourceLabel_.setText("Starts on", juce::dontSendNotification);
  attackLabel_.setText("Attack", juce::dontSendNotification);
  decayLabel_.setText("Decay", juce::dontSendNotification);
  stepCountLabel_.setText("Steps", juce::dontSendNotification);
  rangeLabel_.setText(lang::tr(u8("Moves between")), juce::dontSendNotification);  // was "Sweeps" (Diogo: "sweeps é o quê?")
  range_.setBorderSize({});
  range_.setColour(juce::Label::textColourId, kText);
  range_.setTooltip(lang::tr(u8("The lowest and highest value the destination reaches while it moves (from its own value, after its automation)")));
  addAndMakeVisible(range_);

  for (size_t i = 0; i < shapes().size(); ++i) shape_.addItem(u8(shapes()[i].second), (int)i + 1);
  shape_.onChange = [this] {
    const int i = shape_.getSelectedId() - 1;
    if (i < 0) return;
    change(u8("LFO shape"), [i](json& m) { m["shape"] = shapes()[(size_t)i].first; });
  };
  for (size_t i = 0; i < rates().size(); ++i) rate_.addItem(u8(rates()[i].second), (int)i + 1);
  rate_.onChange = [this] {
    const int i = rate_.getSelectedId() - 1;
    if (i < 0) return;
    change(mode_ == Mode::Step ? u8("Step length") : u8("LFO rate"), [i](json& m) { m["rateBeats"] = rates()[(size_t)i].first; });
  };
  destination_.setTooltip(u8("The parameter it moves"));
  destination_.onChange = [this] {
    const int i = destination_.getSelectedId() - 1;
    if (i < 0 || i >= (int)dests_.size()) return;
    const Destination dest = dests_[(size_t)i];
    change(u8("Modulation destination"), [dest](json& m) {
      m["param"] = dest.param;
      m["depth"] = dest.octaves ? 1.0 : dest.span / 4;  // a new destination starts with a quarter of its range
    });
  };
  source_.setTooltip(u8("Which notes start it: this track's own, or another track's (e.g. every kick)"));
  source_.onChange = [this] {
    const int i = source_.getSelectedId() - 1;
    if (i < 0 || i >= (int)sources_.size()) return;
    const std::string src = sources_[(size_t)i];
    change(u8("Envelope trigger"), [src](json& m) { if (src.empty()) m.erase("source"); else m["source"] = src; });
  };
  for (size_t i = 0; i < stepCounts().size(); ++i) stepCount_.addItem(juce::String(stepCounts()[i]), (int)i + 1);
  stepCount_.setTooltip(u8("How many steps before it repeats"));
  stepCount_.onChange = [this] {
    const int i = stepCount_.getSelectedId() - 1;
    if (i < 0) return;
    const size_t n = (size_t)stepCounts()[(size_t)i];
    change(u8("Number of steps"), [n](json& m) {
      std::vector<double> v = stepsOf(m);
      const std::vector<double> old = v.empty() ? std::vector<double>{0.0} : v;
      v.resize(n);
      for (size_t k = old.size(); k < n; ++k) v[k] = old[k % old.size()];  // more steps: the pattern repeats
      m["steps"] = v;
    });
  };

  auto bar = [this](juce::Slider& s, const char* gesture, const char* key) {
    s.setSliderStyle(juce::Slider::LinearBar);
    s.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);  // drag sideways to change
    s.onDragStart = [this, gesture] { host_.startGesture(u8(gesture)); };
    s.onValueChange = [this, &s, gesture, key] {
      const double v = s.getValue();
      const std::string k = key;
      change(u8(gesture), [v, k](json& m) { m[k] = v; });
    };
    s.onDragEnd = [this] { host_.finishGesture(); };
  };
  bar(depth_, "Modulation depth", "depth");
  depth_.setDoubleClickReturnValue(true, 0.0);
  bar(phase_, "LFO phase", "phase");
  phase_.setRange(0.0, 1.0, 0.01);
  phase_.setDoubleClickReturnValue(true, 0.0);
  phase_.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 360)) + juce::String::fromUTF8("°"); };
  phase_.valueFromTextFunction = [](const juce::String& t) { return t.retainCharacters("0123456789.").getDoubleValue() / 360; };
  phase_.setTooltip(u8("Where the cycle starts (on the song's grid). Drag to change, double-click to type, Alt+click: 0°."));
  bar(attack_, "Envelope attack", "attackMs");
  attack_.setRange(0.0, 2000.0, 1.0);
  attack_.setSkewFactorFromMidPoint(60.0);
  attack_.setDoubleClickReturnValue(true, 5.0);
  attack_.textFromValueFunction = [](double v) { return ms(v); };
  attack_.valueFromTextFunction = [](const juce::String& t) { return t.retainCharacters("0123456789.").getDoubleValue() * (t.containsIgnoreCase("ms") ? 1 : t.containsChar('s') ? 1000 : 1); };
  attack_.setTooltip(u8("How long it takes to get to the top after each note. 0 = at once. Alt+click: 5 ms."));
  bar(decay_, "Envelope decay", "decayMs");
  decay_.setRange(1.0, 10000.0, 1.0);
  decay_.setSkewFactorFromMidPoint(400.0);
  decay_.setDoubleClickReturnValue(true, 250.0);
  decay_.textFromValueFunction = [](double v) { return ms(v); };
  decay_.valueFromTextFunction = attack_.valueFromTextFunction;
  decay_.setTooltip(u8("How long it takes to fall back. Short = a click or a pluck, long = a slow swell down. Alt+click: 250 ms."));
  for (juce::Component* c : std::initializer_list<juce::Component*>{&shape_, &rate_, &destination_, &source_, &stepCount_, &depth_, &phase_, &attack_, &decay_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
  documentChanged();
}

ModulationPanel::~ModulationPanel() = default;

void ModulationPanel::visibilityChanged() {
  if (isVisible()) startTimerHz(30); else stopTimer();
}

void ModulationPanel::timerCallback() {
  if (!isShowing()) return;
  const json* m = chosen();
  const doc::Document& d = host_.currentDocument();
  const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_);
  double where = -1, w = 0;
  if (m && t) {
    const live::Now n = live::at(d, *t, *m, host_.playheadTick());
    if (n.active) { where = n.where; w = n.w; }
  }
  if (std::fabs(where - liveWhere_) < 1e-4 && std::fabs(w - liveW_) < 1e-4) return;
  liveWhere_ = where;
  liveW_ = w;
  repaint(chartArea().expanded(6));
}

void ModulationPanel::setTarget(const std::string& trackId) {
  if (trackId != trackId_) chosen_.clear();
  trackId_ = trackId;
  documentChanged();
}

void ModulationPanel::setMode(Mode mode) {
  if (mode == mode_) return;
  mode_ = mode;
  chosen_.clear();
  lfoTab_.setToggleState(mode == Mode::Lfo, juce::dontSendNotification);
  envTab_.setToggleState(mode == Mode::Env, juce::dontSendNotification);
  stepTab_.setToggleState(mode == Mode::Step, juce::dontSendNotification);
  documentChanged();
  resized();
}

std::vector<ModulationPanel::Destination> ModulationPanel::destinations() const {
  std::vector<Destination> out;
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  if (!t) return out;
  // The engine accepts the instrument's own parameters (built-in only), the volume and the pan.
  if (t->instrument.kind == "builtin")
    if (const doc::InstrumentInfo* info = doc::findInstrumentInfo(t->instrument.name))
      for (const auto& p : info->params) {
        if (!p.automatable) continue;
        const bool hz = std::string(p.unit) == "Hz";
        out.push_back({p.name, u8(p.label), hz ? juce::String("oct") : u8(p.unit), hz ? 8.0 : p.max - p.min, hz});
      }
  out.push_back({"gainDb", u8("Volume"), "dB", 24, false});
  out.push_back({"pan", u8("Pan"), "", 1, false});
  return out;
}

const ModulationPanel::Destination* ModulationPanel::destinationOf(const std::string& param) const {
  for (const auto& d : dests_) if (d.param == param) return &d;
  return nullptr;
}

const json* ModulationPanel::chosen() const {
  for (const auto& l : lfos_) if (l.value("id", "") == chosen_) return &l;
  return nullptr;
}

void ModulationPanel::addLfo() {
  setMode(Mode::Lfo);
  addModulator();
}

void ModulationPanel::addModulator() {
  if (dests_.empty() || total_ >= doc::kMaxModulators) return;
  // The first parameter in Hz (a cutoff), else the first one.
  const auto hz = std::find_if(dests_.begin(), dests_.end(), [](const Destination& d) { return d.octaves; });
  const Destination dest = hz != dests_.end() ? *hz : dests_.front();
  std::string id;
  const std::string track = trackId_;
  if (mode_ == Mode::Env)  // a pluck: up 2 octaves at each note, back in a quarter of a second
    host_.applyEdit([&](doc::Document& d) { id = doc::setEnvelopeModulator(d, track, "", dest.param, 5, 250, dest.octaves ? 2.0 : dest.span / 4); },
                    u8("Add envelope"));
  else if (mode_ == Mode::Step)  // eight sixteenths, a simple up-and-down pattern to start drawing from
    host_.applyEdit([&](doc::Document& d) {
      id = doc::setStepModulator(d, track, "", dest.param, 0.25, {1, 0, 0.5, 0, 1, 0, 0.75, 0.25}, dest.octaves ? 1.0 : dest.span / 4);
    }, u8("Add steps"));
  else  // a quarter of its range, a quarter note
    host_.applyEdit([&](doc::Document& d) { id = doc::setModulator(d, track, "", dest.param, "sine", 1, dest.octaves ? 1.0 : dest.span / 4, 0); },
                    u8("Add LFO"));
  if (!id.empty()) { chosen_ = id; documentChanged(); }
}

void ModulationPanel::modulate(Mode mode, const std::string& param) {
  setMode(mode);
  const Destination* dest = destinationOf(param);
  if (!dest || total_ >= doc::kMaxModulators || lfos_.size() >= kPerTab) return;
  const Destination d0 = *dest;
  const std::string track = trackId_;
  const double depth = d0.octaves ? (mode == Mode::Env ? 2.0 : 1.0) : d0.span / 4;
  std::string id;
  if (mode == Mode::Env)
    host_.applyEdit([&](doc::Document& d) { id = doc::setEnvelopeModulator(d, track, "", d0.param, 5, 250, depth); }, u8("Add envelope"));
  else if (mode == Mode::Step)
    host_.applyEdit([&](doc::Document& d) { id = doc::setStepModulator(d, track, "", d0.param, 0.25, {1, 0, 0.5, 0, 1, 0, 0.75, 0.25}, depth); },
                    u8("Add steps"));
  else
    host_.applyEdit([&](doc::Document& d) { id = doc::setModulator(d, track, "", d0.param, "sine", 1, depth, 0); }, u8("Add LFO"));
  if (!id.empty()) { chosen_ = id; documentChanged(); }
}

void ModulationPanel::setDestination(const std::string& modulatorId, const std::string& param) {
  const Destination* dest = destinationOf(param);
  if (!dest) return;
  const Destination d0 = *dest;
  chosen_ = modulatorId;
  if (const json* m = chosen(); !m || m->value("param", "") == param) { updateControls(); repaint(); return; }
  change(u8("Modulation destination"), [d0](json& m) {
    m["param"] = d0.param;
    m["depth"] = d0.octaves ? 1.0 : d0.span / 4;
  });
}

void ModulationPanel::change(const juce::String& label, const std::function<void(json&)>& edit) {
  const std::string track = trackId_, id = chosen_;
  host_.applyEdit([&](doc::Document& d) {
    const doc::Track* t = d.findTrack(track);
    if (!t) return;
    for (const auto& m : t->modulators) {
      if (m.value("id", "") != id) continue;
      json v = m;
      edit(v);
      const std::string param = v.value("param", ""), shape = v.value("shape", "sine");
      const double depth = v.value("depth", 0.0);
      if (shape == "envelope") {
        const std::string source = v.value("source", "");
        doc::setEnvelopeModulator(d, track, id, param, v.value("attackMs", 0.0), v.value("decayMs", 150.0), depth);
        doc::setModulatorSource(d, track, id, source);
      } else if (shape == "steps") {
        doc::setStepModulator(d, track, id, param, v.value("rateBeats", 0.25), stepsOf(v), depth);
      } else {
        doc::setModulator(d, track, id, param, shape, v.value("rateBeats", 1.0), depth, v.value("phase", 0.0));
      }
      return;
    }
  }, label);
}

void ModulationPanel::documentChanged() {
  identifyWithTrack(*this, host_.currentDocument(), trackId_);  // knobs and bars in the track's colour
  const doc::Document& d = host_.currentDocument();
  const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_);
  dests_ = destinations();
  lfos_ = json::array();
  total_ = t ? (int)t->modulators.size() : 0;
  if (t) for (const auto& m : t->modulators) if (modeOf(m) == mode_) lfos_.push_back(m);
  if (!chosen()) chosen_ = lfos_.empty() ? std::string() : lfos_.front().value("id", "");
  // The tracks whose notes can start an envelope.
  sources_ = {""};
  source_.clear(juce::dontSendNotification);
  source_.addItem(u8("Its own notes"), 1);
  for (const auto& o : d.tracks)
    if (o.id != trackId_ && doc::playsPatterns(o)) {
      sources_.push_back(o.id);
      source_.addItem(u8("Each note of ") + u8(o.name), (int)sources_.size());
    }
  const juce::String trackName = t ? u8(t->name) : juce::String();
  empty_.setText(!t                      ? u8("The master has no modulation: choose a track's strip.")
                 : !lfos_.empty()        ? juce::String()
                 : mode_ == Mode::Env    ? u8("No envelope on ") + trackName + u8(". Click «+ Add envelope» to make a parameter jump at every note and fall back.")
                 : mode_ == Mode::Step   ? u8("No steps on ") + trackName + u8(". Click «+ Add steps» to draw a value for every step, in time with the song.")
                                         : u8("No LFO on ") + trackName + u8(". Click «+ Add LFO» to move a parameter in time with the song."),
                 juce::dontSendNotification);
  updateControls();
  repaint();
}

void ModulationPanel::updateControls() {
  const json* m = chosen();
  int index = 0;
  for (size_t i = 0; i < lfos_.size(); ++i) if (&lfos_[i] == m) index = (int)i;
  const bool shown = m != nullptr;
  const bool lfo = mode_ == Mode::Lfo, env = mode_ == Mode::Env, step = mode_ == Mode::Step;
  for (juce::Component* c : std::initializer_list<juce::Component*>{&name_, &destination_, &depth_, &remove_, &depthLabel_, &destinationLabel_, &rangeLabel_, &range_})
    c->setVisible(shown);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&shape_, &shapeLabel_, &phase_, &phaseLabel_}) c->setVisible(shown && lfo);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&rate_, &rateLabel_}) c->setVisible(shown && (lfo || step));
  for (juce::Component* c : std::initializer_list<juce::Component*>{&source_, &sourceLabel_, &attack_, &attackLabel_, &decay_, &decayLabel_}) c->setVisible(shown && env);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&stepCount_, &stepCountLabel_}) c->setVisible(shown && step);
  remove_.setTooltip(u8("Remove this ") + (env ? u8("envelope") : step ? u8("step sequence") : u8("LFO")));
  rateLabel_.setText(step ? "Step" : "Rate", juce::dontSendNotification);
  rate_.setTooltip(step ? u8("How long each step lasts, in note lengths (synced to the tempo)") : u8("One cycle, in note lengths (synced to the tempo)"));
  empty_.setVisible(!shown);
  if (!shown) return;
  const std::string param = m->value("param", ""), shape = m->value("shape", "sine"), source = m->value("source", "");
  const double rate = m->value("rateBeats", 1.0), depth = m->value("depth", 0.0), phase = m->value("phase", 0.0);
  juce::String what = u8("SYNCED");
  if (env) {
    what = u8("EACH NOTE");
    if (const doc::Track* s = source.empty() ? nullptr : host_.currentDocument().findTrack(source)) what = u8("EACH NOTE OF ") + u8(s->name).toUpperCase();
  }
  name_.setText(juce::String(prefix(mode_)) + " " + juce::String(index + 1) + juce::String::fromUTF8("  —  ") + what, juce::dontSendNotification);
  for (size_t i = 0; i < shapes().size(); ++i) if (shapes()[i].first == shape) shape_.setSelectedId((int)i + 1, juce::dontSendNotification);
  size_t nearest = 0;
  for (size_t i = 0; i < rates().size(); ++i)
    if (std::fabs(std::log2(rates()[i].first / rate)) < std::fabs(std::log2(rates()[nearest].first / rate))) nearest = i;
  rate_.setSelectedId((int)nearest + 1, juce::dontSendNotification);
  destination_.clear(juce::dontSendNotification);
  for (size_t i = 0; i < dests_.size(); ++i) destination_.addItem(dests_[i].label, (int)i + 1);
  const Destination* dest = destinationOf(param);
  if (dest) destination_.setSelectedId((int)(dest - dests_.data()) + 1, juce::dontSendNotification);
  else destination_.setText(u8(param) + " (unknown)", juce::dontSendNotification);
  const double span = dest ? dest->span : 1;
  if (!depth_.isMouseButtonDown()) {
    depth_.setRange(-span, span, span >= 10 ? 0.1 : 0.01);
    depth_.setTextValueSuffix(dest && dest->unit.isNotEmpty() ? " " + dest->unit : juce::String());
    depth_.setValue(depth, juce::dontSendNotification);
  }
  depth_.setTooltip(env ? u8("How far it jumps at each note (below 0: it dips instead). Alt+click: 0.")
                    : dest && dest->octaves ? u8("How far it moves, in octaves either way. Alt+click: 0.")
                                            : u8("How far it moves either way. Alt+click: 0."));
  if (!phase_.isMouseButtonDown()) {
    phase_.setValue(phase, juce::dontSendNotification);
    phase_.updateText();  // in degrees
  }
  if (!attack_.isMouseButtonDown()) { attack_.setValue(m->value("attackMs", 0.0), juce::dontSendNotification); attack_.updateText(); }
  if (!decay_.isMouseButtonDown()) { decay_.setValue(m->value("decayMs", 150.0), juce::dontSendNotification); decay_.updateText(); }
  for (size_t i = 0; i < sources_.size(); ++i) if (sources_[i] == source) source_.setSelectedId((int)i + 1, juce::dontSendNotification);
  const int count = (int)stepsOf(*m).size();
  for (size_t i = 0; i < stepCounts().size(); ++i) if (stepCounts()[i] == count) stepCount_.setSelectedId((int)i + 1, juce::dontSendNotification);
  if (std::find(stepCounts().begin(), stepCounts().end(), count) == stepCounts().end()) stepCount_.setText(juce::String(count), juce::dontSendNotification);
  // The range it sweeps from the parameter's own value: an envelope goes one way (0..1), the others both (-1..1).
  double wLo = -1, wHi = 1;
  if (env) { wLo = 0; wHi = 1; }
  if (step) {
    const auto v = stepsOf(*m);
    if (!v.empty()) { wLo = *std::min_element(v.begin(), v.end()); wHi = *std::max_element(v.begin(), v.end()); }
  }
  if (depth < 0) std::swap(wLo, wHi);
  juce::String sweep = u8("—");
  const doc::Track* t = host_.currentDocument().findTrack(trackId_);
  auto between = [&](double lo, double hi, const juce::String& unit) {
    if (lo > hi) std::swap(lo, hi);
    return number(lo, unit) + juce::String::fromUTF8(" … ") + number(hi, unit);
  };
  if (t && dest) {
    if (const doc::InstrumentParamInfo* p = doc::findInstrumentParam(*t, param)) {
      const double base = doc::instrumentParamValue(*t, *p);
      auto at = [&](double w) { return std::clamp(dest->octaves ? base * std::exp2(depth * w) : base + depth * w, p->min, p->max); };
      if (std::string(p->unit).empty() && p->min >= 0 && p->max <= 1)  // a 0..1 knob reads as %, like on the knob
        sweep = between(at(wLo) * 100, at(wHi) * 100, "%");
      else
        sweep = between(at(wLo), at(wHi), u8(p->unit));
    } else if (param == "gainDb") {
      sweep = between(std::clamp(t->mix.gainDb + depth * wLo, -60.0, 12.0), std::clamp(t->mix.gainDb + depth * wHi, -60.0, 12.0), "dB");
    } else if (param == "pan") {
      sweep = between(std::clamp(t->mix.pan + depth * wLo, -1.0, 1.0), std::clamp(t->mix.pan + depth * wHi, -1.0, 1.0), "");
    }
  }
  range_.setText(sweep, juce::dontSendNotification);
}

juce::Rectangle<int> ModulationPanel::listArea() const { return getLocalBounds().reduced(10, 8).withTrimmedTop(34).withWidth(200); }

juce::Rectangle<int> ModulationPanel::chartArea() const {
  auto r = getLocalBounds().reduced(10, 8).withTrimmedTop(34).withTrimmedLeft(212);
  r = r.withTrimmedTop(26);
  return r.withWidth(std::min(360, r.getWidth() / 3));
}

void ModulationPanel::paint(juce::Graphics& g) {
  g.setColour(kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
  g.setColour(kOutline);
  g.fillRect(10, 8 + 30, getWidth() - 20, 1);
  // The tab's modulators, then "+ Add ...".
  auto list = listArea();
  g.setColour(kOutline);
  g.fillRect(list.getRight() + 5, list.getY(), 1, list.getHeight());
  if (!trackId_.empty()) {
    g.setFont(juce::FontOptions(theme::kFontBody));
    for (size_t i = 0; i < lfos_.size() && list.getHeight() >= kRow; ++i) {
      const auto row = list.removeFromTop(kRow).reduced(0, 1).toFloat();
      const bool on = lfos_[i].value("id", "") == chosen_;
      if (on) {
        g.setColour(kAccent.withAlpha(0.28f));
        g.fillRoundedRectangle(row, theme::kCorner);
      }
      g.setColour(on ? kAccent : kDim);
      g.fillEllipse(row.getX() + 8, row.getCentreY() - 3, 6, 6);
      const Destination* dest = destinationOf(lfos_[i].value("param", ""));
      auto text = row.withTrimmedLeft(22);
      // Every LFO's wave on its own row, side by side with the others (Diogo 2026-11-09 "so vejo um lfo de cada vez?").
      if (mode_ == Mode::Lfo) {
        const auto pic = text.removeFromRight(58.0f).reduced(2.0f, 4.0f);
        g.setColour(kField);
        g.fillRect(pic);
        const std::string shape = lfos_[i].value("shape", "sine");
        const double phase = lfos_[i].value("phase", 0.0), depth = lfos_[i].value("depth", 0.0);
        const float amp = (pic.getHeight() / 2 - 1) * (depth < 0 ? -1.0f : 1.0f);
        juce::Path mini;
        for (int k = 0; k <= 40; ++k) {
          const float px = pic.getX() + pic.getWidth() * (float)k / 40.0f;
          const float py = pic.getCentreY() - amp * (float)lfoAt(shape, 2.0 * k / 40.0 + phase);
          if (k == 0) mini.startNewSubPath(px, py); else mini.lineTo(px, py);
        }
        g.setColour(on ? kAccent : kMuted);
        g.strokePath(mini, juce::PathStrokeType(1.2f));
      }
      g.setColour(on ? kText : kMuted);
      g.drawText(lang::tr(juce::String(prefix(mode_)) + " " + juce::String((int)i + 1) + juce::String::fromUTF8("  →  ") +
                     (dest ? dest->label : u8(lfos_[i].value("param", "")))),
                 text, juce::Justification::centredLeft, true);
    }
    if (list.getHeight() >= kRow) {
      const bool full = lfos_.size() >= kPerTab || total_ >= doc::kMaxModulators;
      const juce::String add = mode_ == Mode::Env ? u8("+ Add envelope") : mode_ == Mode::Step ? u8("+ Add steps") : u8("+ Add LFO");
      g.setColour(full ? kDim : kMuted);
      g.drawText(lang::tr(!full ? add : lfos_.size() >= kPerTab ? juce::String((int)kPerTab) + u8(" here (the most)") : u8("16 on this track (the most)")),
                 list.removeFromTop(kRow).withTrimmedLeft(8), juce::Justification::centredLeft, true);
    }
  }
  const json* m = chosen();
  if (!m) return;
  const auto chart = chartArea().toFloat();
  g.setColour(kField);
  g.fillRoundedRectangle(chart, theme::kCorner);
  g.setColour(kOutline);
  g.drawRoundedRectangle(chart, theme::kCorner, 1);
  const double depth = m->value("depth", 0.0);
  const float sign = depth < 0 ? -1.0f : 1.0f;
  g.setFont(juce::FontOptions(theme::kFontSmall));
  if (mode_ == Mode::Env) {
    // One note's envelope: up over the attack, down over the decay (to -60 dB). Time to the right.
    const double a = m->value("attackMs", 0.0), dcy = m->value("decayMs", 150.0), window = std::max(50.0, (a + dcy) * 1.15);
    const float base = sign > 0 ? chart.getBottom() - 6 : chart.getY() + 6, h = (chart.getHeight() - 12) * sign;
    g.setColour(kOutline);
    g.fillRect(chart.getX(), base, chart.getWidth(), 1.0f);
    juce::Path p;
    const int steps = std::max(2, (int)chart.getWidth());
    for (int i = 0; i <= steps; ++i) {
      const double t = window * i / steps;
      const float px = chart.getX() + chart.getWidth() * (float)i / (float)steps, py = base - h * (float)envAt(a, dcy, t);
      if (i == 0) p.startNewSubPath(px, base); else p.lineTo(px, py);
    }
    g.setColour(kAccent);
    g.strokePath(p, juce::PathStrokeType(1.8f));
    g.setColour(kDim);
    g.drawText(lang::tr(u8("one note, ") + ms(window)), chart.reduced(6, 3), juce::Justification::topRight, false);
    if (liveWhere_ >= 0) {
      const float px = chart.getX() + chart.getWidth() * (float)liveWhere_, py = base - h * (float)liveW_;
      g.setColour(kAccentBright.withAlpha(0.35f));
      g.fillRect(px, chart.getY() + 3, 1.0f, chart.getHeight() - 6);
      g.setColour(kAccentBright);
      g.fillEllipse(px - 4, py - 4, 8, 8);
    }
    return;
  }
  g.fillRect(chart.getX(), chart.getCentreY(), chart.getWidth(), 1.0f);
  if (mode_ == Mode::Step) {
    // The steps as bars from the middle line; click or drag in here to draw them.
    const auto v = stepsOf(*m);
    const float w = chart.getWidth() / (float)std::max<size_t>(1, v.size()), amp = chart.getHeight() / 2 - 6;
    for (size_t i = 0; i < v.size(); ++i) {
      const float x = chart.getX() + w * (float)i, y = chart.getCentreY() - amp * (float)v[i] * sign;
      const auto cell = juce::Rectangle<float>(x + 1.5f, std::min(y, chart.getCentreY()), w - 3.0f, std::max(1.5f, std::fabs(y - chart.getCentreY())));
      g.setColour(kAccent.withAlpha(i % 4 == 0 ? 0.85f : 0.6f));
      g.fillRect(cell);
      g.setColour(kAccentBright);
      g.fillRect(x + 1.5f, y - 1.0f, w - 3.0f, 2.0f);
    }
    g.setColour(kDim);
    g.drawText(lang::tr(u8("click or drag to draw")), chart.reduced(6, 3), juce::Justification::topRight, false);
    if (liveWhere_ >= 0) {
      const float px = chart.getX() + chart.getWidth() * (float)liveWhere_, py = chart.getCentreY() - amp * (float)liveW_ * sign;
      g.setColour(kAccentBright.withAlpha(0.35f));
      g.fillRect(px, chart.getY() + 3, 1.0f, chart.getHeight() - 6);
      g.setColour(kAccentBright);
      g.fillEllipse(px - 4, py - 4, 8, 8);
    }
    return;
  }
  // The chosen LFO's wave over two cycles, with its phase.
  g.fillRect(chart.getCentreX(), chart.getY(), 1.0f, chart.getHeight());
  const std::string shape = m->value("shape", "sine");
  const double phase = m->value("phase", 0.0);
  const float amp = (chart.getHeight() / 2 - 6) * sign;
  juce::Path wave;
  const int steps = std::max(2, (int)chart.getWidth());
  for (int i = 0; i <= steps; ++i) {
    const double x = 2.0 * i / steps + phase;
    const float px = chart.getX() + chart.getWidth() * (float)i / (float)steps;
    const float py = chart.getCentreY() - amp * (float)lfoAt(shape, x);
    if (i == 0) wave.startNewSubPath(px, py); else wave.lineTo(px, py);
  }
  g.setColour(kAccent);
  g.strokePath(wave, juce::PathStrokeType(1.8f));
  g.setColour(kDim);
  g.drawText(lang::tr(u8("2 cycles")), chart.reduced(6, 3), juce::Justification::topRight, false);
  if (liveWhere_ >= 0) {
    const float px = chart.getX() + chart.getWidth() * (float)liveWhere_, py = chart.getCentreY() - amp * (float)liveW_;
    g.setColour(kAccentBright.withAlpha(0.35f));
    g.fillRect(px, chart.getY() + 3, 1.0f, chart.getHeight() - 6);
    g.setColour(kAccentBright);
    g.fillEllipse(px - 4, py - 4, 8, 8);
  }
}

void ModulationPanel::resized() {
  auto r = getLocalBounds().reduced(10, 8);
  auto head = r.removeFromTop(28);
  title_.setBounds(head.removeFromLeft(120));
  for (auto* b : {&lfoTab_, &envTab_, &macroTab_, &stepTab_}) {
    b->setBounds(head.removeFromLeft(58).reduced(2, 2));
    head.removeFromLeft(4);
  }
  r.removeFromTop(6);
  r.removeFromLeft(212);
  empty_.setBounds(r);
  auto top = r.removeFromTop(26);
  remove_.setBounds(top.removeFromRight(80).reduced(0, 1));
  name_.setBounds(top);
  const auto chart = chartArea();
  r.removeFromLeft(chart.getWidth() + 16);
  // Controls in columns: caption above, the control below. Each tab shows its own.
  const int col = juce::jlimit(110, 190, r.getWidth() / 6);
  auto column = [&](juce::Label& caption, juce::Component& c, int width) {
    auto a = r.removeFromLeft(width);
    r.removeFromLeft(12);
    caption.setBounds(a.removeFromTop(16));
    c.setBounds(a.removeFromTop(24));
  };
  if (mode_ == Mode::Env) {
    column(sourceLabel_, source_, col);
    column(attackLabel_, attack_, juce::jmin(col, 110));
    column(decayLabel_, decay_, juce::jmin(col, 110));
  } else if (mode_ == Mode::Step) {
    column(rateLabel_, rate_, juce::jmin(col, 110));
    column(stepCountLabel_, stepCount_, 70);
  } else {
    column(shapeLabel_, shape_, col);
    column(rateLabel_, rate_, juce::jmin(col, 110));
  }
  column(destinationLabel_, destination_, col);
  column(depthLabel_, depth_, col);
  if (mode_ == Mode::Lfo) column(phaseLabel_, phase_, juce::jmin(col, 110));
  column(rangeLabel_, range_, juce::jmax(140, r.getWidth()));
}

void ModulationPanel::drawStep(juce::Point<int> at) {
  const json* m = chosen();
  if (!m) return;
  const auto chart = chartArea();
  const size_t n = stepsOf(*m).size();
  if (n == 0) return;
  const int i = juce::jlimit(0, (int)n - 1, (int)((at.x - chart.getX()) * (int)n / std::max(1, chart.getWidth())));
  const float amp = chart.getHeight() / 2.0f - 6;
  double v = juce::jlimit(-1.0, 1.0, (double)((chart.getCentreY() - at.y) / amp));
  if (m->value("depth", 0.0) < 0) v = -v;  // the bars are drawn flipped for a negative depth
  v = std::round(v * 20) / 20;             // in 5% steps, so a flat line is easy to draw
  if (std::fabs(stepsOf(*m)[(size_t)i] - v) < 1e-9) return;
  change(u8("Draw steps"), [i, v](json& j) {
    auto s = stepsOf(j);
    if ((size_t)i < s.size()) s[(size_t)i] = v;
    j["steps"] = s;
  });
}

void ModulationPanel::mouseDown(const juce::MouseEvent& e) {
  pressedRow_ = -1;
  if (const auto list = listArea(); !trackId_.empty() && list.contains(e.getPosition())) {
    const int row = (e.y - list.getY()) / kRow;
    if (row >= 0 && row < (int)lfos_.size()) pressedRow_ = row;
  }
  if (mode_ != Mode::Step || !chosen() || !chartArea().contains(e.getPosition())) return;
  drawing_ = true;
  host_.startGesture(u8("Draw steps"));  // one drag is one Undo
  drawStep(e.getPosition());
}

void ModulationPanel::mouseDrag(const juce::MouseEvent& e) {
  if (drawing_) { drawStep(e.getPosition()); return; }
  // A row dragged out of the list: drop it on a SOUND bar to make it move that control.
  if (pressedRow_ >= 0 && pressedRow_ < (int)lfos_.size() && e.getDistanceFromDragStart() > 5)
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this); c && !c->isDragAndDropActive()) {
      const std::string id = lfos_[(size_t)pressedRow_].value("id", "");
      chosen_ = id;
      updateControls();
      repaint();
      juce::Image ghost(juce::Image::ARGB, 120, 20, true);
      {
        juce::Graphics g(ghost);
        g.setColour(kAccent.withAlpha(0.85f));
        g.fillRoundedRectangle(0, 0, 120, 20, theme::kCorner);
        g.setColour(kText);
        g.setFont(juce::FontOptions(theme::kFontSmall));
        g.drawText(lang::tr(juce::String(prefix(mode_)) + " " + juce::String(pressedRow_ + 1) + juce::String::fromUTF8(" → …")), 6, 0, 110, 20,
                   juce::Justification::centredLeft);
      }
      c->startDragging(juce::String(kDragPrefix) + u8(id), this, juce::ScaledImage(ghost), true);
      pressedRow_ = -2;  // dragging: the mouse-up is not a click on the list
    }
}

void ModulationPanel::mouseUp(const juce::MouseEvent& e) {
  if (pressedRow_ == -2) { pressedRow_ = -1; return; }  // a row dragged onto a bar (or nowhere)
  if (drawing_) {
    drawing_ = false;
    host_.finishGesture();
    return;
  }
  if (trackId_.empty()) return;
  auto list = listArea();
  if (!list.contains(e.getPosition())) return;
  const int row = (e.y - list.getY()) / kRow;
  if (row >= 0 && row < (int)lfos_.size()) {
    chosen_ = lfos_[(size_t)row].value("id", "");
    updateControls();
    repaint();
  } else if (row == (int)lfos_.size() && lfos_.size() < kPerTab) {
    addModulator();
  }
}

} // namespace core::app
