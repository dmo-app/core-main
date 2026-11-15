// The parts the Core FX and Core EQ windows share (CoreFxWindow.h): the editor's ties to its effect in the song
// (values, one undo step per change, a drag as one gesture, A/B), the input spectrum, and the LFOs in time with
// the song. Only CoreFxWindow.cpp and CoreEqWindow.cpp include this.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include "CoreFxWindow.h"
#include "Theme.h"
#include "edit.hpp"

namespace core::app::corefx {

using json = nlohmann::json;

inline juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
inline juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }

constexpr double kMinFreq = 20, kMaxFreq = 20000;
constexpr int kLfos = 2, kMaxDestinations = 4;

inline juce::String formatValue(const doc::EffectParamInfo& p, double v) {
  if (p.step > 0 && !p.choices.empty()) return u8(p.choices[(size_t)std::clamp((int)std::lround(v), 0, (int)p.choices.size() - 1)]);
  const std::string unit = p.unit;
  if (unit == "Hz") return v >= 1000 ? juce::String(v / 1000, v >= 10000 ? 1 : 2) + " kHz" : juce::String((int)std::lround(v)) + " Hz";
  if (unit == "ms") return juce::String(v, v < 10 ? 1 : 0) + " ms";
  if (unit == "dB") return juce::String(v, 1) + " dB";
  if (unit == "s") return juce::String(v, v < 10 ? 2 : 1) + " s";
  if (unit == "oct") return juce::String(v, 1) + " oct";
  if (p.max <= 1.0 && p.min >= 0.0) return juce::String((int)std::lround(v * 100)) + "%";
  return juce::String(v, p.max - p.min <= 20 ? 2 : 1);
}

// The effect (by id and name) in a chain: its index, -1 when it is gone.
inline int indexIn(const doc::Document& d, const std::string& trackId, const std::string& fxId, const std::string& effect) {
  if (!trackId.empty() && !d.findTrack(trackId)) return -1;
  const json& chain = doc::effectChain(d, trackId);
  for (size_t i = 0; i < chain.size(); ++i)
    if (chain[i].is_object() && chain[i].value("id", std::string()) == fxId && doc::effectName(chain[i]) == effect) return (int)i;
  return -1;
}

inline void styleKnob(juce::Slider& s, juce::Colour fill = theme::kAccent) {
  s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
  s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);
  s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  s.setColour(juce::Slider::rotarySliderFillColourId, fill);
}
inline void styleToggle(juce::TextButton& b, const juce::String& text, const juce::String& tip, juce::Colour on = theme::kPrimary) {
  b.setButtonText(text);
  b.setTooltip(tip);
  b.setClickingTogglesState(false);
  b.setColour(juce::TextButton::buttonOnColourId, on);
  b.setWantsKeyboardFocus(false);
}

// LFO shapes as the engine names them, and rates in beats (the song's tempo).
inline const char* const kShapes[] = {"sine", "triangle", "square", "saw", "ramp", "random"};
inline const char* const kShapeLabels[] = {"Sine", "Triangle", "Square", "Saw", "Ramp", "Random"};
inline const double kRates[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
inline const char* const kRateLabels[] = {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"};

inline double wave(const std::string& shape, double ph) {
  ph -= std::floor(ph);
  if (shape == "triangle") return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
  if (shape == "square") return ph < 0.5 ? 1 : -1;
  if (shape == "saw") return 1 - 2 * ph;
  if (shape == "ramp") return 2 * ph - 1;
  if (shape == "random") {  // a held step per eighth of the cycle (a picture of it, not the engine's values)
    const int s = (int)(ph * 8);
    return std::sin(s * 12.9898 + 78.233) * 0.9;
  }
  return std::sin(ph * juce::MathConstants<double>::twoPi);
}

}  // namespace core::app::corefx

namespace core::app {

// ---------------------------------------------------------------- an editor tied to one effect in the song
class CoreFxWindows::Pane : public juce::Component {
public:
  using json = nlohmann::json;
  Pane(CoreFxWindows& owner, std::string trackId, std::string fxId, std::string effect)
      : owner_(owner), trackId_(std::move(trackId)), fxId_(std::move(fxId)), effect_(std::move(effect)), info_(doc::findEffect(effect_)) {}
  ~Pane() override { while (gestures_ > 0) endGesture(); }

  virtual juce::String title() const = 0;  // "Core FX"
  virtual void refresh() = 0;              // everything from the song
  virtual void tick() {}                   // a frame
  virtual void spectrumChanged() {}
  // The values an LFO can move, by group: add(menu, param) puts one in a menu.
  virtual void destinations(juce::PopupMenu& menu, const std::function<void(juce::PopupMenu&, const std::string&)>& add) = 0;
  // How a destination reads in an LFO's line ("03 High Mid · Gain"); by default the parameter's label.
  virtual juce::String destinationName(const std::string& param) const {
    const doc::EffectParamInfo* p = this->param(param);
    return juce::String::fromUTF8(p ? p->label : param.c_str());
  }

  const std::string& trackId() const { return trackId_; }
  const std::string& fxId() const { return fxId_; }
  const std::string& effect() const { return effect_; }
  const doc::Document& song() const { return owner_.host_.currentDocument(); }
  PatternEditor::Host& host() { return owner_.host_; }
  void undo() { if (owner_.cb_.undo) owner_.cb_.undo(); }
  void redo() { if (owner_.cb_.redo) owner_.cb_.redo(); }
  bool alive() const { return corefx::indexIn(song(), trackId_, fxId_, effect_) >= 0; }
  const json* fx() const {
    const int i = corefx::indexIn(song(), trackId_, fxId_, effect_);
    return i < 0 ? nullptr : &doc::effectChain(song(), trackId_)[(size_t)i];
  }
  const doc::EffectInfo* info() const { return info_; }
  const doc::EffectParamInfo* param(const std::string& name) const {
    if (info_) for (const auto& p : info_->params) if (name == p.name) return &p;
    return nullptr;
  }
  double get(const std::string& name) const {
    const doc::EffectParamInfo* p = param(name);
    const json* f = fx();
    return f ? doc::effectParam(*f, name, p ? p->def : 0.0) : (p ? p->def : 0.0);
  }
  bool automated(const std::string& name) const { return doc::findAutomation(song(), trackId_, doc::effectParamKey(fxId_, name)) != nullptr; }
  // One undo step (or a step of the gesture that is open), on this effect wherever it is in the chain now.
  void edit(const std::function<void(doc::Document&, int)>& change, const juce::String& label) {
    const std::string trackId = trackId_, fxId = fxId_, effect = effect_;
    host().applyEdit([&](doc::Document& d) {
      const int i = corefx::indexIn(d, trackId, fxId, effect);
      if (i >= 0) change(d, i);
    }, label);
  }
  void set(const std::string& name, double v, const juce::String& label = {}) {
    edit([&](doc::Document& d, int i) { doc::setEffectParam(d, trackId_, i, name, v); }, label.isEmpty() ? title() : label);
  }
  void setMany(const std::vector<std::pair<std::string, double>>& values, const juce::String& label) {
    edit([&](doc::Document& d, int i) { for (const auto& [n, v] : values) doc::setEffectParam(d, trackId_, i, n, v); }, label);
  }
  void setBypass(bool bypass) {
    edit([&](doc::Document& d, int i) { doc::setEffectBypass(d, trackId_, i, bypass); }, bypass ? u8("Effect off") : u8("Effect on"));
  }
  void beginGesture() { host().startGesture(title()); ++gestures_; }
  void endGesture() { if (gestures_ > 0) { --gestures_; host().finishGesture(); } }

  // A knob or bar bound to one parameter: the value from the song, a drag is one gesture.
  void bind(juce::Slider& s, const std::string& name) {
    const doc::EffectParamInfo* p = param(name);
    if (!p) return;
    s.setRange(p->min, p->max, p->step > 0 ? p->step : 0.0);
    if (p->logScale && p->min > 0) s.setSkewFactorFromMidPoint(std::sqrt(p->min * p->max));
    s.setDoubleClickReturnValue(true, p->def);
    s.textFromValueFunction = [p](double v) { return corefx::formatValue(*p, v); };
    s.updateText();
    s.setTooltip(u8(p->label) + (p->hint ? u8(": ") + u8(p->hint) : juce::String()) + u8(" (double-click: default)"));
    s.setWantsKeyboardFocus(false);
    s.onDragStart = [this] { beginGesture(); };
    s.onValueChange = [this, &s, name] { set(name, s.getValue()); };
    s.onDragEnd = [this] { endGesture(); };
    bound_.push_back({&s, name});
  }
  void unbind(juce::Slider& s) {
    bound_.erase(std::remove_if(bound_.begin(), bound_.end(), [&](const auto& b) { return b.first == &s; }), bound_.end());
  }
  void refreshBound() {
    for (auto& [s, name] : bound_) {
      if (!s->isMouseButtonDown()) s->setValue(get(name), juce::dontSendNotification);
      s->setEnabled(!automated(name));  // a value on a curve follows it
    }
  }

  // A/B: the values of each slot (null: not taken yet); your changes stay in the one that is lit.
  int slot() const { return slot_; }
  void compare(int slot) {
    const json* f = fx();
    if (!f || slot == slot_) return;
    const json now = f->value("params", json::object());
    slots_[(size_t)slot_] = now;
    if (slots_[(size_t)slot].is_null()) slots_[(size_t)slot] = now;  // B starts as a copy of A
    const json next = slots_[(size_t)slot];
    slot_ = slot;
    const std::string trackId = trackId_;
    edit([&](doc::Document& d, int i) { doc::effectChain(d, trackId)[(size_t)i]["params"] = next; }, slot == 0 ? u8("Compare A") : u8("Compare B"));
    refresh();
  }
  void copyToOther() {  // the lit slot's values into the other one
    const json* f = fx();
    if (f) slots_[(size_t)(1 - slot_)] = f->value("params", json::object());
  }

  void setSpectrum(std::vector<float> bands) {
    if (bands == spectrum_) return;
    spectrum_ = std::move(bands);
    spectrumChanged();
  }
  const std::vector<float>& spectrum() const { return spectrum_; }

  // The track's modulators of this effect's LFOs start with this ("cfx_<effect id>_l1_").
  std::string lfoPrefix(int n) const { return std::string(effect_ == "eq" ? "ceq_" : "cfx_") + fxId_ + "_l" + std::to_string(n + 1) + "_"; }

protected:
  static juce::String u8(const char* t) { return corefx::u8(t); }
  static juce::String u8(const std::string& t) { return corefx::u8(t); }

  CoreFxWindows& owner_;
  std::string trackId_, fxId_, effect_;
  const doc::EffectInfo* info_ = nullptr;
  int gestures_ = 0, slot_ = 0;
  std::vector<float> spectrum_;
  std::vector<std::pair<juce::Slider*, std::string>> bound_;
  std::array<json, 2> slots_;
};

// ---------------------------------------------------------------- an LFO in time with the song
// Its destinations are the track's modulators "<prefix><k>" on "fx:<effect id>:<param>", one per destination,
// sharing the LFO's shape and rate. The master has no modulators: there it says so.
class CoreFxWindows::Lfo final : public juce::Component {
public:
  using json = nlohmann::json;
  Lfo(Pane& e, int n, juce::Colour colour = theme::kAccent) : e_(e), n_(n), colour_(colour) {
    using namespace corefx;
    title_.setText(u8("LFO ") + juce::String(n + 1), juce::dontSendNotification);
    title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colour_.interpolatedWith(juce::Colours::white, 0.25f));
    for (int i = 0; i < 6; ++i) shape_.addItem(u8(kShapeLabels[i]), i + 1);
    for (int i = 0; i < 8; ++i) rate_.addItem(u8(kRateLabels[i]), i + 1);
    shape_.setTooltip(u8("The LFO's shape"));
    rate_.setTooltip(u8("How fast, on the song's tempo"));
    shape_.onChange = [this] { if (shape_.getSelectedId() > 0) { shapeName_ = kShapes[shape_.getSelectedId() - 1]; rewrite(corefx::u8("LFO shape")); } };
    rate_.onChange = [this] { if (rate_.getSelectedId() > 0) { rateBeats_ = kRates[rate_.getSelectedId() - 1]; rewrite(corefx::u8("LFO rate")); } };
    add_.setButtonText(u8("+"));
    add_.setTooltip(u8("Choose a value for this LFO to move"));
    add_.onClick = [this] { showAddMenu(); };
    add_.setColour(juce::TextButton::buttonColourId, colour_.withAlpha(0.22f).overlaidWith(theme::kCard.withAlpha(0.4f)));
    add_.setColour(juce::TextButton::textColourOffId, theme::kText);
    title_.setColour(juce::Label::textColourId, colour_.interpolatedWith(juce::Colours::white, 0.25f));
    for (juce::Component* c : std::initializer_list<juce::Component*>{&title_, &shape_, &rate_, &add_}) { c->setWantsKeyboardFocus(false); addAndMakeVisible(c); }
    shape_.setSelectedId(1, juce::dontSendNotification);
    rate_.setSelectedId(5, juce::dontSendNotification);
  }

  bool here() const { return !e_.trackId().empty(); }

  // The track's modulators of this LFO, in their order.
  std::vector<json> mine() const {
    std::vector<json> out;
    if (!here()) return out;
    const doc::Track* t = e_.song().findTrack(e_.trackId());
    if (!t) return out;
    const std::string pre = e_.lfoPrefix(n_);
    for (const auto& m : t->modulators)
      if (m.is_object() && m.value("id", std::string()).rfind(pre, 0) == 0) out.push_back(m);
    return out;
  }
  static std::string paramOf(const json& m) {  // "fx:<id>:<param>" -> "<param>"
    const std::string key = m.value("param", std::string());
    const auto at = key.rfind(':');
    return at == std::string::npos ? key : key.substr(at + 1);
  }

  void refresh(std::vector<std::string>& modulated) {
    using namespace corefx;
    const auto mods = mine();
    on_ = mods.empty() || mods.front().value("on", true) != false;
    if (!mods.empty()) {
      shapeName_ = mods.front().value("shape", std::string("sine"));
      rateBeats_ = mods.front().value("rateBeats", 4.0);
    }
    for (int i = 0; i < 6; ++i) if (shapeName_ == kShapes[i]) shape_.setSelectedId(i + 1, juce::dontSendNotification);
    int best = 0;
    for (int i = 0; i < 8; ++i) if (std::abs(kRates[i] - rateBeats_) < std::abs(kRates[best] - rateBeats_)) best = i;
    rate_.setSelectedId(best + 1, juce::dontSendNotification);
    std::string shape;
    for (const auto& m : mods) shape += m.value("id", std::string()) + "=" + m.value("param", std::string()) + ";";
    if (shape != shapeKey_) {  // other destinations: rows again
      shapeKey_ = shape;
      rows_.clear();
      for (const auto& m : mods) addRow(m);
      resized();
    }
    for (auto& row : rows_) {
      for (const auto& m : mods)
        if (m.value("id", std::string()) == row->id && !row->depth.isMouseButtonDown())
          row->depth.setValue(m.value("depth", 0.0), juce::dontSendNotification);
      modulated.push_back(row->param);
    }
    resized();
    repaint();
  }
  int count() const { return (int)rows_.size(); }
  juce::String summary() const {  // "Triangle · 1/4"
    using namespace corefx;
    juce::String s;
    for (int i = 0; i < 6; ++i) if (shapeName_ == kShapes[i]) s = u8(kShapeLabels[i]);
    int best = 0;
    for (int i = 0; i < 8; ++i) if (std::abs(kRates[i] - rateBeats_) < std::abs(kRates[best] - rateBeats_)) best = i;
    return s + u8(" · ") + u8(kRateLabels[best]);
  }

  void paint(juce::Graphics& g) override {
    using namespace corefx;
    const auto r = getLocalBounds().toFloat();
    g.setColour(theme::kCard);
    g.fillRoundedRectangle(r, theme::kCorner);
    // The LFO's bar: its title, shape and rate, in its colour.
    const auto h = r.withHeight((float)kHeader);
    // A see-through tint of the LFO's colour with a 2 px line on top, like the EQ's cards (Diogo, 2026-11-09).
    g.setColour(colour_.withAlpha(0.10f));  // barely a tint: the title carries the colour
    juce::Path top;
    top.addRoundedRectangle(h.getX(), h.getY(), h.getWidth(), h.getHeight(), theme::kCorner, theme::kCorner, true, true, false, false);
    g.fillPath(top);
    g.setColour(colour_);
    g.fillRect(h.getX(), h.getY(), h.getWidth(), 2.0f);
    g.setColour(colour_);  // a border in the LFO's colour, like the EQ's cards, and a line under the bar
    g.drawRoundedRectangle(r.reduced(1.0f), theme::kCorner, 2.0f);
    g.fillRect(h.getX(), h.getBottom() - 1.0f, h.getWidth(), 1.0f);
    const auto w = waveArea().toFloat();
    g.setColour(theme::kField);
    g.fillRoundedRectangle(w, theme::kCorner);
    juce::Path p;
    for (int i = 0; i <= 80; ++i) {
      const float x = w.getX() + w.getWidth() * i / 80.0f, y = w.getCentreY() - (float)wave(shapeName_, i / 80.0) * (w.getHeight() / 2 - 4);
      if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
    }
    g.setColour(rows_.empty() ? theme::kDim : colour_);
    g.strokePath(p, juce::PathStrokeType(1.5f));
    if (!rows_.empty()) {  // where the LFO is now (the playhead)
      const double ph = e_.host().playheadTick() / (double)doc::kPpq / std::max(1.0 / 64, rateBeats_);
      const double frac = ph - std::floor(ph);
      const float x = w.getX() + w.getWidth() * (float)frac, y = w.getCentreY() - (float)wave(shapeName_, frac) * (w.getHeight() / 2 - 4);
      g.setColour(theme::kText);
      g.fillEllipse(x - 3.5f, y - 3.5f, 7, 7);
    }
    if (here()) {  // at the right corner: how many connections, and the LFO on or off
      const juce::Colour ink = theme::kText;
      g.setColour(ink.withAlpha(0.85f));
      g.setFont(juce::FontOptions(theme::kFontSmall));
      const int n = (int)rows_.size();
      g.drawText(juce::String(n) + (n == 1 ? corefx::u8(" connection") : corefx::u8(" connections")), countArea_, juce::Justification::centredRight, false);
      // On / off: a light circle, full when on, empty when off.
      const auto d = onArea_.toFloat().withSizeKeepingCentre(14, 14);
      const juce::Colour light = ink == theme::kText ? colour_.interpolatedWith(juce::Colours::white, 0.6f) : ink;
      if (on_) { g.setColour(light); g.fillEllipse(d); }
      g.setColour(light);
      g.drawEllipse(d.reduced(0.75f), 1.5f);
    }
    if (!here()) {
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawText(u8("Only on tracks"), rowsArea(), juce::Justification::centred, true);
    }
  }
  void resized() override {
    auto top = getLocalBounds().removeFromTop(kHeader).reduced(8, 3);
    onArea_ = top.removeFromRight(20).withSizeKeepingCentre(16, 16);
    top.removeFromRight(6);
    countArea_ = top.removeFromRight(here() ? 92 : 0);
    top.removeFromRight(6);
    title_.setBounds(top.removeFromLeft(70));
    rate_.setBounds(top.removeFromRight(80).reduced(1, 0));
    top.removeFromRight(4);
    shape_.setBounds(top.removeFromRight(96).reduced(1, 0));
    auto rows = rowsArea();
    for (auto& row : rows_) {
      auto line = rows.removeFromTop(24);
      row->remove.setBounds(line.removeFromRight(24).reduced(1));
      row->name.setBounds(line.removeFromLeft(std::max(150, line.getWidth() * 3 / 5)));
      row->depth.setBounds(line.reduced(2, 1));
    }
    // A big + for another destination: the whole space when there is none yet.
    add_.setVisible(here() && rows_.size() < (size_t)corefx::kMaxDestinations);
    if (!rows_.empty()) rows.removeFromTop(4);
    // No connection yet: the + fills the space; otherwise a small + under the lines.
    add_.setBounds(rows_.empty() ? rows : rows.removeFromTop(24).removeFromLeft(32));
  }
  void tick() { if (!rows_.empty()) repaint(waveArea()); }
  void mouseDown(const juce::MouseEvent& m) override {
    if (!here() || !onArea_.expanded(4).contains(m.getPosition())) return;
    const auto mods = mine();
    if (mods.empty()) return;
    const bool on = !on_;
    const std::string trackId = e_.trackId();
    e_.host().applyEdit([&](doc::Document& d) {
      for (const auto& mod : mods) doc::setModulatorOn(d, trackId, mod.value("id", std::string()), on);
    }, on ? corefx::u8("LFO on") : corefx::u8("LFO off"));
  }

private:
  // One connection, read as a line: "LFO 1 -> 03 High Mid · Gain", the destination the most visible.
  struct Link final : juce::Component {
    juce::String source, destination;
    juce::Colour colour;
    void paint(juce::Graphics& g) override {
      auto r = getLocalBounds();
      g.setColour(colour);
      g.fillRect(r.removeFromLeft(3).reduced(0, 3));
      r.removeFromLeft(6);
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
      const int sw = juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontSmall, juce::Font::bold)), source) + 4;
      g.drawText(source, r.removeFromLeft(sw), juce::Justification::centredLeft, false);
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawText(juce::String::fromUTF8("\u2192"), r.removeFromLeft(18), juce::Justification::centred, false);
      g.setColour(theme::kText);
      g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
      g.drawText(destination, r, juce::Justification::centredLeft, true);
    }
  };
  // A plain x, no button around it.
  struct Cross final : juce::Button {
    Cross() : juce::Button("remove") {}
    void paintButton(juce::Graphics& g, bool over, bool) override {
      const auto c = getLocalBounds().toFloat().getCentre();
      const float a = 4.5f;
      g.setColour(over ? theme::kText : theme::kMuted);
      g.drawLine(c.x - a, c.y - a, c.x + a, c.y + a, 1.6f);
      g.drawLine(c.x - a, c.y + a, c.x + a, c.y - a, 1.6f);
    }
  };
  struct Row { std::string id, param; Link name; juce::Slider depth{juce::Slider::LinearBar, juce::Slider::TextBoxLeft}; Cross remove; };

  static constexpr int kHeader = 30;
  juce::Rectangle<int> body() const { return getLocalBounds().withTrimmedTop(kHeader).reduced(8, 8); }
  juce::Rectangle<int> waveArea() const { auto b = body(); return b.removeFromLeft(juce::jlimit(110, 260, b.getWidth() * 2 / 5)); }
  juce::Rectangle<int> rowsArea() const { auto b = body(); return b.withTrimmedLeft(waveArea().getWidth() + 10); }

  static double depthRange(const doc::EffectParamInfo& p) { return std::string(p.unit) == "Hz" ? 4.0 : p.max - p.min; }

  void addRow(const json& m) {
    using corefx::u8;
    auto row = std::make_unique<Row>();
    row->id = m.value("id", std::string());
    row->param = paramOf(m);
    const doc::EffectParamInfo* p = e_.param(row->param);
    row->name.source = u8("LFO ") + juce::String(n_ + 1);
    row->name.destination = e_.destinationName(row->param);
    row->name.colour = colour_;
    row->name.setInterceptsMouseClicks(false, false);
    const double range = p ? depthRange(*p) : 1.0;
    row->depth.setRange(-range, range, 0.0);
    row->depth.setDoubleClickReturnValue(true, 0.0);
    const bool hz = p && std::string(p->unit) == "Hz";
    row->depth.textFromValueFunction = [hz, p](double v) {
      if (hz) return (v > 0 ? "+" : "") + juce::String(v, 2) + " oct";
      return (v > 0 ? "+" : "") + (p ? corefx::formatValue(*p, v) : juce::String(v, 2));
    };
    row->depth.setTooltip(u8("How far it moves this value, either way (double-click: none)"));
    row->depth.setColour(juce::Slider::trackColourId, colour_.withAlpha(0.6f));
    row->depth.setWantsKeyboardFocus(false);
    Row* raw = row.get();
    row->depth.onDragStart = [this] { e_.beginGesture(); };
    row->depth.onValueChange = [this, raw] { write(raw->id, raw->param, raw->depth.getValue(), corefx::u8("LFO depth")); };
    row->depth.onDragEnd = [this] { e_.endGesture(); };
    row->remove.setTooltip(u8("Remove this destination"));
    row->remove.setWantsKeyboardFocus(false);
    const std::string id = row->id, trackId = e_.trackId();
    juce::Component::SafePointer<Lfo> self(this);
    row->remove.onClick = [self, id, trackId] {  // after the click: the rows are built again
      juce::MessageManager::callAsync([self, id, trackId] {
        if (self) self->e_.host().applyEdit([&](doc::Document& d) { doc::removeModulator(d, trackId, id); }, corefx::u8("Remove LFO destination"));
      });
    };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&row->name, &row->depth, &row->remove}) addAndMakeVisible(c);
    rows_.push_back(std::move(row));
  }

  void write(const std::string& id, const std::string& name, double depth, const juce::String& label) {
    const std::string trackId = e_.trackId(), key = doc::effectParamKey(e_.fxId(), name), shape = shapeName_;
    const double rate = rateBeats_;
    e_.host().applyEdit([&](doc::Document& d) { doc::setModulator(d, trackId, id, key, shape, rate, depth, 0); }, label);
  }
  void rewrite(const juce::String& label) {  // the shape or rate, on every destination
    const auto mods = mine();
    if (mods.empty()) { repaint(); return; }
    const std::string trackId = e_.trackId(), shape = shapeName_;
    const double rate = rateBeats_;
    e_.host().applyEdit([&](doc::Document& d) {
      for (const auto& m : mods)
        doc::setModulator(d, trackId, m.value("id", std::string()), m.value("param", std::string()), shape, rate, m.value("depth", 0.0), 0);
    }, label);
  }

  void showAddMenu() {
    juce::PopupMenu menu;
    std::vector<std::string> names;
    e_.destinations(menu, [&](juce::PopupMenu& to, const std::string& name) {
      const doc::EffectParamInfo* p = e_.param(name);
      if (!p || p->step > 0 || !p->automatable) return;
      bool taken = false;
      for (const auto& row : rows_) taken |= row->param == name;
      names.push_back(name);
      to.addItem((int)names.size(), corefx::u8(p->label), !taken && !e_.automated(name));
    });
    juce::Component::SafePointer<Lfo> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_), [self, names](int id) {
      if (!self || id <= 0 || id > (int)names.size()) return;
      self->addDestination(names[(size_t)id - 1]);
    });
  }
  void addDestination(const std::string& name) {
    const auto mods = mine();
    std::string id;
    for (int k = 1; k <= corefx::kMaxDestinations && id.empty(); ++k) {
      const std::string candidate = e_.lfoPrefix(n_) + std::to_string(k);
      if (std::none_of(mods.begin(), mods.end(), [&](const json& m) { return m.value("id", std::string()) == candidate; })) id = candidate;
    }
    if (id.empty()) return;
    const doc::EffectParamInfo* p = e_.param(name);
    write(id, name, p ? depthRange(*p) * 0.25 : 0.25, corefx::u8("LFO destination"));
  }

  Pane& e_;
  int n_;
  juce::Colour colour_;
  std::string shapeName_ = "sine", shapeKey_ = "-";
  juce::Rectangle<int> onArea_, countArea_;
  bool on_ = true;
  double rateBeats_ = 4;
  juce::Label title_;
  juce::ComboBox shape_, rate_;
  // A big "+" drawn as two strokes, so it reads from across the window.
  struct Plus : juce::TextButton {
    void paintButton(juce::Graphics& g, bool over, bool down) override {
      getLookAndFeel().drawButtonBackground(g, *this, findColour(buttonColourId), over, down);
      const float side = juce::jlimit(10.0f, 34.0f, (float)std::min(getWidth(), getHeight()) * 0.45f);
      const auto c = getLocalBounds().toFloat().getCentre();
      g.setColour(findColour(textColourOffId).withAlpha(over ? 1.0f : 0.85f));
      const float t = std::max(2.0f, side / 9.0f);
      g.fillRect(juce::Rectangle<float>(side, t).withCentre(c));
      g.fillRect(juce::Rectangle<float>(t, side).withCentre(c));
    }
  } add_;
  std::vector<std::unique_ptr<Row>> rows_;
};

// Core EQ's editor (CoreEqWindow.cpp).
CoreFxWindows::Pane* makeCoreEqEditor(CoreFxWindows& owner, const std::string& trackId, const std::string& fxId);

}  // namespace core::app
