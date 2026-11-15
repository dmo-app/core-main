#include "CoreFxWindow.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include "CoreFxPane.h"
#include "Language.h"
#include "Theme.h"
#include "TrackColour.h"
#include "edit.hpp"

namespace core::app {

using json = nlohmann::json;
using namespace corefx;

namespace {
using theme::kPanel, theme::kCard, theme::kField, theme::kText, theme::kMuted, theme::kDim, theme::kOutline, theme::kAccent;

constexpr int kModules = 5, kRegions = 4;
constexpr double kFullWidth = 7.99;  // the engine's: a region this wide is the whole spectrum

struct ModuleDef { const char* id; const char* label; juce::uint32 colour; };
// Rule 11 (ui-colours.md, Diogo 2026-11-09 "está lindo"): the modules in a scale of frost tones, light first and darker
// down the chain. The tone goes on the header (~35%), the 2 px border and the knobs.
const ModuleDef kMods[kModules] = {{"sat", "Saturate", 0xffb8ffff}, {"rev", "Reverb", 0xff8eeff7}, {"del", "Delay", 0xff66dceb},
                                   {"mod", "Modulate", 0xff45c2db}, {"lofi", "Lo-fi", 0xff2fa3c4}};
juce::Colour modColour(int m) { return juce::Colour(kMods[m].colour); }
std::string mod(int m, const char* key) { return std::string(kMods[m].id) + key; }
std::string region(int m, int r, const char* key) { return std::string(kMods[m].id) + "R" + std::to_string(r + 1) + key; }

// "satDrive" is the Saturate module's own; "satR2Freq" is one of its regions (R and a digit).
bool ownKnob(const std::string& n, int m) {
  const size_t b = std::strlen(kMods[m].id);
  if (n.size() <= b || n.compare(0, b, kMods[m].id) != 0) return false;
  return !(n[b] == 'R' && n.size() > b + 1 && std::isdigit((unsigned char)n[b + 1]));
}


// The engine's region band-pass (CoreFx::regionResponse): 0..1, 1 at the centre; the whole spectrum at the top width.
double response(double freq, double centre, double widthOct) {
  if (widthOct >= kFullWidth) return 1.0;
  const double w = std::pow(2.0, std::clamp(widthOct, 0.1, 10.0));
  const double q = std::sqrt(w) / (w - 1.0), x = freq / std::max(1.0, centre);
  const double d = q * (x - 1.0 / x);
  return 1.0 / std::sqrt(1.0 + d * d);
}

}  // namespace

// ---------------------------------------------------------------- the editor in one window
class CoreFxWindows::Editor final : public CoreFxWindows::Pane {
public:
  class Card;
  class Spectrum;

  Editor(CoreFxWindows& owner, std::string trackId, std::string fxId);

  juce::String title() const override { return u8("Core FX"); }
  void destinations(juce::PopupMenu& menu, const std::function<void(juce::PopupMenu&, const std::string&)>& add) override;
  void spectrumChanged() override;

  // Locks (for Vary): a module ("sat": its type and knobs) or one region ("satR2"). Kept in the effect's "locks".
  bool locked(const std::string& key) const {
    const json* f = fx();
    if (!f || !f->contains("locks") || !(*f)["locks"].is_array()) return false;
    for (const auto& k : (*f)["locks"]) if (k.is_string() && k.get<std::string>() == key) return true;
    return false;
  }
  void toggleLock(const std::string& key) {
    const bool was = locked(key);
    const std::string trackId = trackId_;
    edit([&](doc::Document& d, int i) {
      json& f = doc::effectChain(d, trackId)[(size_t)i];
      json locks = json::array();
      if (f.contains("locks") && f["locks"].is_array())
        for (const auto& k : f["locks"]) if (k.is_string() && k.get<std::string>() != key) locks.push_back(k);
      if (!was) locks.push_back(key);
      if (locks.empty()) f.erase("locks"); else f["locks"] = locks;
    }, was ? u8("Unlock") : u8("Lock"));
  }

  int selected() const { return selected_; }
  void select(int m);
  void refresh() override;
  void tick() override;  // a frame: the LFOs' dots
  void vary();

  void paint(juce::Graphics& g) override {
    g.fillAll(kPanel);
  }
  void paintOverChildren(juce::Graphics& g) override {
    // The track's colour around the window (ui-colours.md rule 1): a 3 px line on top and a 2 px border.
    const juce::Colour c = theme::identityOf(*this, theme::kBlueSoft);
    g.setColour(c);
    g.fillRect(getLocalBounds().removeFromTop(3));
    g.drawRect(getLocalBounds(), 2);
  }
  void resized() override;

private:
  int selected_ = 0;
  // Top bar.
  juce::ComboBox preset_;
  juce::TextButton undo_, redo_, a_, b_, bypass_, fxOnly_, limiter_, vary_;
  juce::Slider amount_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft}, output_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
  juce::Label amountLabel_, outputLabel_, hint_;
  // The psytrance wash (#236): the delay's echoes into the reverb, and the dry level.
  juce::Slider wash_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft}, dry_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
  juce::Label washLabel_, dryLabel_;
  std::vector<const doc::EffectPreset*> presets_;
  std::unique_ptr<Spectrum> view_;
  std::array<std::unique_ptr<Card>, kModules> cards_;
  std::array<std::unique_ptr<Lfo>, kLfos> lfos_;
};

// ---------------------------------------------------------------- the spectrum and the regions
class CoreFxWindows::Editor::Spectrum final : public juce::Component, public juce::SettableTooltipClient {
public:
  explicit Spectrum(Editor& e) : e_(e) {
    setRepaintsOnMouseActivity(false);
    setTooltip(u8("Where each module takes its sound from. Drag a point: frequency (left-right) and how much (up-down). "
                  "Wheel over a point: width. Double-click: add a region to the chosen module, or remove the point. Right-click a point: more."));
  }

  juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced(34, 10).withTrimmedBottom(10); }
  float xOf(double f) const {
    const auto p = plot();
    return p.getX() + (float)(std::log2(std::clamp(f, kMinFreq, kMaxFreq) / kMinFreq) / std::log2(kMaxFreq / kMinFreq)) * p.getWidth();
  }
  double freqAt(float x) const {
    const auto p = plot();
    return std::clamp(kMinFreq * std::pow(kMaxFreq / kMinFreq, (double)((x - p.getX()) / p.getWidth())), kMinFreq, kMaxFreq);
  }
  float yOf(double amount) const { const auto p = plot(); return p.getBottom() - (float)std::clamp(amount, 0.0, 1.0) * p.getHeight(); }
  double amountAt(float y) const { const auto p = plot(); return std::clamp((double)((p.getBottom() - y) / p.getHeight()), 0.0, 1.0); }
  float yOfDb(double db) const { const auto p = plot(); return p.getBottom() - (float)std::clamp((db + 84.0) / 84.0, 0.0, 1.0) * p.getHeight(); }

  bool shown(int m, int r) const { return e_.get(region(m, r, "Amount")) > 0.001 || (dragM_ == m && dragR_ == r); }
  juce::Point<float> node(int m, int r) const { return {xOf(e_.get(region(m, r, "Freq"))), yOf(e_.get(region(m, r, "Amount")))}; }
  bool on(int m) const { return e_.get(mod(m, "On")) > 0.5; }

  // The point under the mouse: the chosen module's first, then the others that are on.
  std::pair<int, int> hit(juce::Point<float> at) const {
    std::vector<int> order = {e_.selected()};
    for (int m = 0; m < kModules; ++m) if (m != e_.selected() && on(m)) order.push_back(m);
    for (int m : order)
      for (int r = 0; r < kRegions; ++r)
        if (shown(m, r) && node(m, r).getDistanceFrom(at) <= 9.0f) return {m, r};
    return {-1, -1};
  }

  void paint(juce::Graphics& g) override {
    const auto p = plot();
    g.setColour(kField);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
      const float x = xOf(f);
      g.setColour(theme::kGrid);
      g.fillRect(x, p.getY(), 1.0f, p.getHeight());
      g.setColour(kDim);
      g.drawText(f >= 1000 ? juce::String((int)(f / 1000)) + "k" : juce::String((int)f), (int)x - 16, (int)p.getBottom() + 1, 32, 12,
                 juce::Justification::centred, false);
    }
    for (int k = 0; k <= 4; ++k) {
      const float y = yOf(k / 4.0);
      g.setColour(theme::kGrid);
      g.fillRect(p.getX(), y, p.getWidth(), 1.0f);
      g.setColour(kDim);
      g.drawText(juce::String(k * 25) + "%", 0, (int)y - 6, 30, 12, juce::Justification::centredRight, false);
    }
    // The effect's input (the window in front): 1/6-octave bands from 20 Hz.
    const auto& bands = e_.spectrum();
    if (!bands.empty()) {
      juce::Path fill;
      fill.startNewSubPath(p.getX(), p.getBottom());
      for (size_t i = 0; i < bands.size(); ++i) {
        const double f = kMinFreq * std::pow(2.0, ((double)i + 0.5) / 6.0);
        if (f > kMaxFreq) break;
        fill.lineTo(xOf(f), yOfDb(bands[i]));
      }
      fill.lineTo(p.getRight(), p.getBottom());
      fill.closeSubPath();
      g.setColour(theme::kPrimary.withAlpha(0.45f));
      g.fillPath(fill);
      g.setColour(theme::kPrimaryLight.withAlpha(0.7f));
      g.strokePath(fill, juce::PathStrokeType(1.0f));
    } else {
      g.setColour(kDim);
      g.drawText(u8("Play the song to see what comes into this effect"), p.reduced(8).toNearestInt(), juce::Justification::topRight, false);
    }
    // Each module that is on (and the chosen one): the sum of its regions, as the engine sends it.
    std::vector<int> order;
    for (int m = 0; m < kModules; ++m) if (m != e_.selected() && on(m)) order.push_back(m);
    order.push_back(e_.selected());
    for (int m : order) {
      const bool chosen = m == e_.selected(), active = on(m);
      double freq[kRegions], amount[kRegions], width[kRegions];
      for (int r = 0; r < kRegions; ++r) {
        freq[r] = e_.get(region(m, r, "Freq"));
        amount[r] = e_.get(region(m, r, "Amount"));
        width[r] = e_.get(region(m, r, "Width"));
      }
      juce::Path curve;
      const int steps = std::max(2, (int)p.getWidth() / 3);
      for (int s = 0; s <= steps; ++s) {
        const float x = p.getX() + p.getWidth() * (float)s / (float)steps;
        const double f = freqAt(x);
        double sum = 0;
        for (int r = 0; r < kRegions; ++r) sum += amount[r] * response(f, freq[r], width[r]);
        const float y = yOf(std::min(1.0, sum));
        if (s == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
      }
      const juce::Colour c = modColour(m).withAlpha(active ? 1.0f : 0.45f);
      if (chosen) {
        juce::Path area(curve);
        area.lineTo(p.getRight(), p.getBottom());
        area.lineTo(p.getX(), p.getBottom());
        area.closeSubPath();
        g.setColour(c.withAlpha(0.14f));
        g.fillPath(area);
      }
      g.setColour(c.withMultipliedAlpha(chosen ? 1.0f : 0.6f));
      g.strokePath(curve, juce::PathStrokeType(chosen ? 2.0f : 1.3f));
      for (int r = 0; r < kRegions; ++r) {
        if (!shown(m, r)) continue;
        const auto n = node(m, r);
        if (chosen && width[r] < kFullWidth) {  // the band between its -3 dB points
          const double half = std::pow(2.0, width[r] / 2);
          g.setColour(c.withAlpha(0.5f));
          g.fillRect(juce::Rectangle<float>(xOf(freq[r] / half), n.y - 1, xOf(freq[r] * half) - xOf(freq[r] / half), 2.0f));
        }
        const float rad = chosen ? 7.0f : 5.0f;
        g.setColour(c);
        g.fillEllipse(n.x - rad, n.y - rad, rad * 2, rad * 2);
        if (e_.locked(region(m, r, ""))) {
          g.setColour(kText);
          g.drawEllipse(n.x - rad - 2.5f, n.y - rad - 2.5f, rad * 2 + 5, rad * 2 + 5, 1.2f);
        }
        if (chosen) {
          g.setColour(theme::kBackground);
          g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
          g.drawText(juce::String(r + 1), juce::Rectangle<float>(n.x - rad, n.y - rad, rad * 2, rad * 2), juce::Justification::centred, false);
        }
      }
    }
    g.setColour(kOutline);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), theme::kCorner, 1.0f);
  }

  void mouseDown(const juce::MouseEvent& ev) override {
    const auto [m, r] = hit(ev.position);
    if (m < 0) return;
    if (m != e_.selected()) e_.select(m);
    if (ev.mods.isPopupMenu()) { showMenu(m, r); return; }
    dragM_ = m;
    dragR_ = r;
    e_.beginGesture();
  }
  void mouseDrag(const juce::MouseEvent& ev) override {
    if (dragM_ < 0) return;
    e_.setMany({{region(dragM_, dragR_, "Freq"), freqAt(ev.position.x)}, {region(dragM_, dragR_, "Amount"), amountAt(ev.position.y)}}, u8("Core FX region"));
  }
  void mouseUp(const juce::MouseEvent&) override {
    if (dragM_ < 0) return;
    dragM_ = dragR_ = -1;
    e_.endGesture();
    repaint();
  }
  void mouseDoubleClick(const juce::MouseEvent& ev) override {
    const auto [m, r] = hit(ev.position);
    if (m >= 0) { e_.set(region(m, r, "Amount"), 0, u8("Remove region")); return; }
    const int sel = e_.selected();
    for (int k = 0; k < kRegions; ++k)
      if (!shown(sel, k)) {
        std::vector<std::pair<std::string, double>> v = {{region(sel, k, "Freq"), freqAt(ev.position.x)},
                                                          {region(sel, k, "Amount"), std::max(0.05, amountAt(ev.position.y))}};
        if (e_.get(region(sel, k, "Width")) >= kFullWidth) v.push_back({region(sel, k, "Width"), 2.0});
        if (!on(sel)) v.push_back({mod(sel, "On"), 1.0});
        e_.setMany(v, u8("Add region"));
        return;
      }
  }
  void mouseWheelMove(const juce::MouseEvent& ev, const juce::MouseWheelDetails& w) override {
    const auto [m, r] = hit(ev.position);
    if (m < 0 || w.deltaY == 0) return;
    const std::string key = region(m, r, "Width");
    const double next = std::clamp(e_.get(key) - (w.deltaY > 0 ? 0.25 : -0.25), 0.3, 8.0);
    e_.beginGesture();
    e_.set(key, next, u8("Region width"));
    e_.endGesture();
  }

private:
  void showMenu(int m, int r) {
    juce::PopupMenu menu, widths;
    const double w = e_.get(region(m, r, "Width"));
    const double choices[] = {0.5, 1, 2, 3, 4, 6, 8};
    for (int i = 0; i < 7; ++i)
      widths.addItem(100 + i, choices[i] >= 8 ? u8("Whole spectrum") : juce::String(choices[i], 1) + u8(" octaves"), true, std::abs(w - choices[i]) < 0.05);
    menu.addSubMenu(u8("Width"), widths);
    menu.addItem(1, u8("Lock this region (Vary leaves it)"), true, e_.locked(region(m, r, "")));
    menu.addItem(2, u8("Remove"));
    juce::Component::SafePointer<Spectrum> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [self, m, r, choices](int id) {
      if (!self) return;
      Editor& e = self->e_;
      if (id == 1) e.toggleLock(region(m, r, ""));
      else if (id == 2) e.set(region(m, r, "Amount"), 0, u8("Remove region"));
      else if (id >= 100 && id < 107) e.set(region(m, r, "Width"), choices[id - 100], u8("Region width"));
    });
  }

  Editor& e_;
  int dragM_ = -1, dragR_ = -1;
};

// ---------------------------------------------------------------- one module
class CoreFxWindows::Editor::Card final : public juce::Component {
public:
  Card(Editor& e, int m) : e_(e), m_(m) {
    title_.setText(u8(kMods[m].label), juce::dontSendNotification);
    title_.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    title_.setInterceptsMouseClicks(false, false);
    styleToggle(on_, u8("On"), u8("This module on or off (its tail rings out)"));
    styleToggle(solo_, u8("S"), u8("Hear this module alone"), theme::kBlueLight);
    styleToggle(lock_, u8("Lock"), u8("Vary leaves this module's type and knobs as they are"), theme::kPrimary);
    styleToggle(fit_, u8("Fit"), u8("Filters what comes back with the same regions, so it stays where it was sent from"));
    on_.onClick = [this] { e_.set(mod(m_, "On"), on_.getToggleState() ? 0.0 : 1.0, on_.getToggleState() ? u8("Module off") : u8("Module on")); };
    solo_.onClick = [this] { e_.set("solo", solo_.getToggleState() ? 0.0 : (double)(m_ + 1), u8("Solo")); };
    lock_.onClick = [this] { e_.toggleLock(kMods[m_].id); };
    fit_.onClick = [this] { e_.set(mod(m_, "Fit"), fit_.getToggleState() ? 0.0 : 1.0, u8("Fit to regions")); };
    if (const doc::EffectParamInfo* p = e_.param(mod(m_, "Type")))
      for (size_t i = 0; i < p->choices.size(); ++i) type_.addItem(u8(p->choices[i]), (int)i + 1);
    type_.setTooltip(u8("What this module does"));
    type_.setWantsKeyboardFocus(false);
    type_.onChange = [this] { if (type_.getSelectedId() > 0) e_.set(mod(m_, "Type"), type_.getSelectedId() - 1, u8("Module type")); };
    const auto& ps = e_.info()->params;
    for (const auto& p : ps) {  // the module's two own knobs: the ones after On and Type
      const std::string n = p.name;
      if (!ownKnob(n, m_)) continue;
      const std::string rest = n.substr(std::strlen(kMods[m_].id));
      if (rest == "On" || rest == "Type" || rest == "Return" || rest == "Fit") continue;
      knobs_[count_].name = n;
      knobs_[count_].label.setText(u8(p.label), juce::dontSendNotification);
      if (++count_ == 2) break;
    }
    knobs_[2].name = mod(m_, "Return");
    knobs_[2].label.setText(u8("Return"), juce::dontSendNotification);
    count_ = 3;
    for (int k = 0; k < count_; ++k) {
      styleKnob(knobs_[k].knob);
      knobs_[k].knob.setColour(juce::Slider::rotarySliderFillColourId, modColour(m_));
      e_.bind(knobs_[k].knob, knobs_[k].name);
      knobs_[k].label.setFont(juce::FontOptions(theme::kFontSmall));
      knobs_[k].label.setJustificationType(juce::Justification::centred);
      knobs_[k].label.setColour(juce::Label::textColourId, kMuted);
      knobs_[k].label.setInterceptsMouseClicks(false, false);
      addAndMakeVisible(knobs_[k].label);
      addAndMakeVisible(knobs_[k].knob);
    }
    for (juce::Component* c : std::initializer_list<juce::Component*>{&title_, &on_, &solo_, &lock_, &fit_, &type_}) addAndMakeVisible(c);
  }

  void refresh(const std::vector<std::string>& modulated) {
    const bool isOn = e_.get(mod(m_, "On")) > 0.5;
    on_.setToggleState(isOn, juce::dontSendNotification);
    solo_.setToggleState((int)std::lround(e_.get("solo")) == m_ + 1, juce::dontSendNotification);
    lock_.setToggleState(e_.locked(kMods[m_].id), juce::dontSendNotification);
    fit_.setToggleState(e_.get(mod(m_, "Fit")) > 0.5, juce::dontSendNotification);
    type_.setSelectedId((int)std::lround(e_.get(mod(m_, "Type"))) + 1, juce::dontSendNotification);
    for (int k = 0; k < count_; ++k) {
      auto& kn = knobs_[k];
      if (!kn.knob.isMouseButtonDown()) kn.knob.setValue(e_.get(kn.name), juce::dontSendNotification);
      kn.knob.setEnabled(!e_.automated(kn.name));  // a value on a curve follows it
      const bool lfo = std::find(modulated.begin(), modulated.end(), kn.name) != modulated.end();
      kn.label.setColour(juce::Label::textColourId, lfo ? kAccent : kMuted);
      kn.label.setTooltip(lfo ? u8("Moved by an LFO") : juce::String());
    }
    repaint();
  }

  void paint(juce::Graphics& g) override {
    const bool isOn = on_.getToggleState(), chosen = e_.selected() == m_;
    const auto r = getLocalBounds().toFloat();
    const juce::Colour tone = modColour(m_);
    g.setColour(kCard.withAlpha(isOn ? 1.0f : 0.6f));
    g.fillRoundedRectangle(r, theme::kCorner);
    g.setColour(kCard.interpolatedWith(tone, 0.35f).withAlpha(isOn ? 1.0f : 0.6f));  // the header in the module's tone
    g.fillRect(r.withHeight(36.0f));
    g.setColour(tone.withAlpha(chosen ? 1.0f : isOn ? 0.6f : 0.3f));  // the 2 px border; the chosen one at full tone
    g.drawRect(r.reduced(1.0f), 2.0f);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(8, 8);
    r.removeFromTop(2);
    auto top = r.removeFromTop(24);
    lock_.setBounds(top.removeFromRight(42).reduced(1));
    solo_.setBounds(top.removeFromRight(24).reduced(1));
    on_.setBounds(top.removeFromRight(34).reduced(1));
    title_.setBounds(top);
    r.removeFromTop(4);
    auto second = r.removeFromTop(24);
    fit_.setBounds(second.removeFromRight(40).reduced(1));
    second.removeFromRight(4);
    type_.setBounds(second.reduced(0, 1));
    r.removeFromTop(6);
    const int w = r.getWidth() / count_;
    for (int k = 0; k < count_; ++k) {
      auto cell = r.withX(r.getX() + k * w).withWidth(w);
      knobs_[k].label.setBounds(cell.removeFromTop(16));
      knobs_[k].knob.setBounds(cell.reduced(2, 0));
    }
  }
  void mouseDown(const juce::MouseEvent&) override { e_.select(m_); }

private:
  struct Knob { std::string name; juce::Label label; juce::Slider knob; };
  Editor& e_;
  int m_, count_ = 0;
  juce::Label title_;
  juce::TextButton on_, solo_, lock_, fit_;
  juce::ComboBox type_;
  std::array<Knob, 3> knobs_;
};

// ---------------------------------------------------------------- the editor
CoreFxWindows::Editor::Editor(CoreFxWindows& owner, std::string trackId, std::string fxId)
    : Pane(owner, std::move(trackId), std::move(fxId), "corefx") {
  int id = 1;
  for (const auto& p : doc::effectPresets())
    if (std::string(p.effect) == "corefx") { preset_.addItem(u8(p.name), id++); presets_.push_back(&p); }
  preset_.setTextWhenNothingSelected(u8("Presets"));
  preset_.setTooltip(u8("A whole sound: every value and region"));
  preset_.onChange = [this] {
    const int i = preset_.getSelectedId() - 1;
    if (i < 0 || i >= (int)presets_.size()) return;
    const doc::EffectPreset* p = presets_[(size_t)i];
    edit([&](doc::Document& d, int at) { doc::applyEffectPreset(d, trackId_, at, *p); }, u8("Preset ") + u8(p->name));
  };
  undo_.setButtonText(u8("Undo"));
  redo_.setButtonText(u8("Redo"));
  undo_.onClick = [this] { undo(); };
  redo_.onClick = [this] { redo(); };
  styleToggle(a_, u8("A"), u8("Compare: the sound in A (your changes stay in the one that is lit)"));
  styleToggle(b_, u8("B"), u8("Compare: the sound in B"));
  a_.onClick = [this] { compare(0); };
  b_.onClick = [this] { compare(1); };
  styleToggle(bypass_, u8("Bypass"), u8("The effect off: the dry sound"), theme::kBlueLight);
  bypass_.onClick = [this] {
    setBypass(!bypass_.getToggleState());
  };
  styleToggle(fxOnly_, u8("FX Only"), u8("Only the effects, without the dry sound (an FX bus)"));
  fxOnly_.onClick = [this] { set("fxOnly", fxOnly_.getToggleState() ? 0.0 : 1.0); };
  styleToggle(limiter_, u8("Limiter"), u8("Keeps the output under -0.3 dB"));
  limiter_.onClick = [this] { set("limiter", limiter_.getToggleState() ? 0.0 : 1.0); };
  vary_.setButtonText(u8("Vary"));
  vary_.setTooltip(u8("A variation: changes types, knobs and regions a little, except what is locked (the lit Locks and the ringed points)"));
  vary_.onClick = [this] { vary(); };
  amountLabel_.setText(u8("Amount"), juce::dontSendNotification);
  outputLabel_.setText(u8("Output"), juce::dontSendNotification);
  washLabel_.setText(u8("Delay into reverb"), juce::dontSendNotification);
  dryLabel_.setText(u8("Dry"), juce::dontSendNotification);
  for (juce::Label* l : {&amountLabel_, &outputLabel_, &washLabel_, &dryLabel_}) {
    l->setColour(juce::Label::textColourId, kMuted);
    l->setFont(juce::FontOptions(theme::kFontBody));
    l->setJustificationType(juce::Justification::centredRight);
  }
  bind(amount_, "amount");
  bind(output_, "output");
  bind(wash_, "delToRev");
  bind(dry_, "dry");
  hint_.setText(u8("Click a module to see its regions. Drag points, wheel for width, double-click to add or remove."), juce::dontSendNotification);
  hint_.setColour(juce::Label::textColourId, kDim);
  hint_.setFont(juce::FontOptions(theme::kFontSmall));
  for (juce::Component* c : std::initializer_list<juce::Component*>{&preset_, &undo_, &redo_, &a_, &b_, &bypass_, &amountLabel_, &amount_, &fxOnly_,
                                                                     &limiter_, &outputLabel_, &output_, &vary_, &hint_,
                                                                     &washLabel_, &wash_, &dryLabel_, &dry_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
  view_ = std::make_unique<Spectrum>(*this);
  addAndMakeVisible(*view_);
  for (int m = 0; m < kModules; ++m) { cards_[(size_t)m] = std::make_unique<Card>(*this, m); addAndMakeVisible(*cards_[(size_t)m]); }
  for (int n = 0; n < kLfos; ++n) { lfos_[(size_t)n] = std::make_unique<Lfo>(*this, n); addAndMakeVisible(*lfos_[(size_t)n]); }
  setSize(1100, 720);
  refresh();
}

void CoreFxWindows::Editor::select(int m) {
  selected_ = std::clamp(m, 0, kModules - 1);
  for (auto& c : cards_) c->repaint();
  view_->repaint();
}

void CoreFxWindows::Editor::refresh() {
  identifyWithTrack(*this, song(), trackId_);  // its own knobs (Amount, Output…) and border in the track's colour
  const json* f = fx();
  if (!f) return;
  bypass_.setToggleState(doc::effectBypassed(*f), juce::dontSendNotification);
  fxOnly_.setToggleState(get("fxOnly") > 0.5, juce::dontSendNotification);
  limiter_.setToggleState(get("limiter") > 0.5, juce::dontSendNotification);
  a_.setToggleState(slot_ == 0, juce::dontSendNotification);
  b_.setToggleState(slot_ == 1, juce::dontSendNotification);
  refreshBound();
  std::vector<std::string> modulated;
  for (auto& l : lfos_) l->refresh(modulated);
  for (auto& c : cards_) c->refresh(modulated);
  view_->repaint();
}

void CoreFxWindows::Editor::spectrumChanged() { view_->repaint(); }

void CoreFxWindows::Editor::destinations(juce::PopupMenu& menu, const std::function<void(juce::PopupMenu&, const std::string&)>& add) {
  add(menu, "amount");
  add(menu, "output");
  add(menu, "delToRev");
  add(menu, "dry");
  for (int m = 0; m < kModules; ++m) {
    juce::PopupMenu sub;
    for (const auto& p : info()->params) {
      const std::string n = p.name;
      if (ownKnob(n, m)) add(sub, n);
    }
    for (int r = 0; r < kRegions; ++r)
      if (get(region(m, r, "Amount")) > 0.001) {
        add(sub, region(m, r, "Freq"));
        add(sub, region(m, r, "Amount"));
      }
    menu.addSubMenu(u8(kMods[m].label), sub);
  }
}

void CoreFxWindows::Editor::tick() { for (auto& l : lfos_) l->tick(); }

void CoreFxWindows::Editor::vary() {
  juce::Random rnd;
  auto spread = [&](double v, double lo, double hi, double part) { return std::clamp(v + (rnd.nextDouble() * 2 - 1) * (hi - lo) * part, lo, hi); };
  std::vector<std::pair<std::string, double>> values;
  for (int m = 0; m < kModules; ++m) {
    if (!locked(kMods[m].id)) {
      for (const auto& p : info()->params) {
        const std::string n = p.name;
        if (!ownKnob(n, m)) continue;
        const std::string rest = n.substr(std::strlen(kMods[m].id));
        if (rest == "On" || rest == "Fit" || automated(n)) continue;
        const double v = get(n);
        if (rest == "Type") { if (rnd.nextFloat() < 0.35f) values.push_back({n, (double)rnd.nextInt((int)p.max + 1)}); continue; }
        if (rest == "Return") { values.push_back({n, spread(v, -6.0, 3.0, 0.15)}); continue; }
        if (p.step > 0) { if (rnd.nextFloat() < 0.4f) values.push_back({n, std::clamp(v + (rnd.nextBool() ? 1 : -1), p.min, p.max)}); continue; }
        if (p.logScale && p.min > 0) values.push_back({n, std::clamp(v * std::pow(2.0, rnd.nextDouble() * 1.2 - 0.6), p.min, p.max)});
        else values.push_back({n, spread(v, p.min, p.max, 0.2)});
      }
    }
    for (int r = 0; r < kRegions; ++r) {
      if (locked(region(m, r, "")) || get(region(m, r, "Amount")) <= 0.001) continue;
      const std::string fq = region(m, r, "Freq"), am = region(m, r, "Amount"), wd = region(m, r, "Width");
      if (!automated(fq)) values.push_back({fq, std::clamp(get(fq) * std::pow(2.0, rnd.nextDouble() * 2 - 1), kMinFreq, kMaxFreq)});
      if (!automated(am)) values.push_back({am, std::clamp(get(am) + rnd.nextDouble() * 0.3 - 0.15, 0.05, 1.0)});
      if (!automated(wd) && get(wd) < kFullWidth) values.push_back({wd, std::clamp(get(wd) + rnd.nextDouble() - 0.5, 0.3, 6.0)});
    }
  }
  if (!values.empty()) setMany(values, u8("Vary"));
}

void CoreFxWindows::Editor::resized() {
  auto r = getLocalBounds().reduced(10);
  auto top = r.removeFromTop(30);
  preset_.setBounds(top.removeFromLeft(200).reduced(1, 2));
  top.removeFromLeft(8);
  undo_.setBounds(top.removeFromLeft(54).reduced(1, 2));
  redo_.setBounds(top.removeFromLeft(54).reduced(1, 2));
  top.removeFromLeft(8);
  a_.setBounds(top.removeFromLeft(30).reduced(1, 2));
  b_.setBounds(top.removeFromLeft(30).reduced(1, 2));
  top.removeFromLeft(8);
  bypass_.setBounds(top.removeFromLeft(64).reduced(1, 2));
  vary_.setBounds(top.removeFromRight(60).reduced(1, 2));
  top.removeFromRight(8);
  output_.setBounds(top.removeFromRight(110).reduced(1, 3));
  outputLabel_.setBounds(top.removeFromRight(52));
  limiter_.setBounds(top.removeFromRight(64).reduced(1, 2));
  fxOnly_.setBounds(top.removeFromRight(68).reduced(1, 2));
  top.removeFromRight(8);
  amount_.setBounds(top.removeFromRight(juce::jlimit(90, 200, top.getWidth() - 60)).reduced(1, 3));
  amountLabel_.setBounds(top.removeFromRight(56));
  r.removeFromTop(4);
  {  // The bottom row: the hint, then Delay into reverb and Dry next to each other.
    auto row = r.removeFromBottom(24);
    dry_.setBounds(row.removeFromRight(120).reduced(1, 2));
    dryLabel_.setBounds(row.removeFromRight(40));
    row.removeFromRight(12);
    wash_.setBounds(row.removeFromRight(120).reduced(1, 2));
    washLabel_.setBounds(row.removeFromRight(130));
    hint_.setBounds(row);
  }
  auto lfo = r.removeFromBottom(130);
  r.removeFromBottom(8);
  const int lw = (lfo.getWidth() - 8) / 2;
  lfos_[0]->setBounds(lfo.removeFromLeft(lw));
  lfos_[1]->setBounds(lfo.removeFromRight(lw));
  auto cards = r.removeFromBottom(176);
  r.removeFromBottom(8);
  const int cw = (cards.getWidth() - 4 * 8) / kModules;
  for (int m = 0; m < kModules; ++m) {
    cards_[(size_t)m]->setBounds(cards.removeFromLeft(cw));
    cards.removeFromLeft(8);
  }
  view_->setBounds(r);
}

// ---------------------------------------------------------------- the window
class CoreFxWindows::Window final : public juce::DocumentWindow {
public:
  Window(CoreFxWindows& owner, const std::string& trackId, const std::string& fxId, const std::string& effect)
      : juce::DocumentWindow(u8("Core"), kPanel, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton), owner_(owner) {
    if (effect == "eq") editor_ = makeCoreEqEditor(owner, trackId, fxId);
    else editor_ = new Editor(owner, trackId, fxId);
    setUsingNativeTitleBar(true);
    setContentOwned(editor_, true);
    setResizable(true, false);
    setResizeLimits(960, 640, 2400, 1600);
    retitle();
  }
  Pane& editor() { return *editor_; }
  void retitle() {
    juce::String where = u8("Master");
    if (const doc::Track* t = editor_->song().findTrack(editor_->trackId())) where = u8(t->name);
    setName(editor_->title() + u8(" · ") + where);
  }
  void closeButtonPressed() override {
    setVisible(false);
    juce::Component::SafePointer<Window> self(this);
    juce::MessageManager::callAsync([self] { if (self) self->owner_.close(self.getComponent()); });
  }
  void activeWindowStatusChanged() override { if (isActiveWindow()) owner_.activated(this); }

private:
  CoreFxWindows& owner_;
  Pane* editor_ = nullptr;
};

// ---------------------------------------------------------------- the windows
CoreFxWindows::CoreFxWindows(PatternEditor::Host& host, Callbacks callbacks) : host_(host), cb_(std::move(callbacks)) {}

CoreFxWindows::~CoreFxWindows() {
  stopTimer();
  watched_ = nullptr;
  windows_.clear();
}

void CoreFxWindows::open(const std::string& trackId, const std::string& effectId) {
  const doc::Document& d = host_.currentDocument();
  const std::string effect = indexIn(d, trackId, effectId, "eq") >= 0 ? "eq" : "corefx";
  if (indexIn(d, trackId, effectId, effect) < 0) return;
  for (auto& w : windows_)
    if (w->editor().trackId() == trackId && w->editor().fxId() == effectId) {
      w->setVisible(true);
      w->toFront(true);
      activated(w.get());
      return;
    }
  auto w = std::make_unique<Window>(*this, trackId, effectId, effect);
  // Each new window a little lower and to the right of the last, so several can be seen at once.
  const int step = 28 * (int)(windows_.size() % 8);
  w->centreWithSize(w->getWidth(), w->getHeight());
  w->setTopLeftPosition(w->getX() + step, w->getY() + step);
  w->setVisible(true);
  windows_.push_back(std::move(w));
  windows_.back()->toFront(true);
  activated(windows_.back().get());
  if (!isTimerRunning()) startTimerHz(30);
}

void CoreFxWindows::documentChanged() {
  for (auto& w : windows_) {
    if (!w->editor().alive()) {  // the effect was removed (or undone): its window goes, after this call
      if (w->isVisible()) w->closeButtonPressed();
      continue;
    }
    w->editor().refresh();
    w->retitle();
  }
}

void CoreFxWindows::closeAll() {
  windows_.clear();
  watched_ = nullptr;
  if (cb_.watch) cb_.watch({}, {});
  stopTimer();
}

void CoreFxWindows::activated(Window* w) {
  if (w == watched_) return;
  watched_ = w;
  watch(w);
}

void CoreFxWindows::watch(Window* w) {
  if (!cb_.watch) return;
  if (!w) { cb_.watch({}, {}); return; }
  const std::string& t = w->editor().trackId();
  w->editor().setSpectrum({});
  cb_.watch(t.empty() ? std::string("master") : t, w->editor().fxId());
}

void CoreFxWindows::close(Window* w) {
  const auto it = std::find_if(windows_.begin(), windows_.end(), [w](const auto& p) { return p.get() == w; });
  if (it == windows_.end()) return;
  const bool wasWatched = watched_ == w;
  windows_.erase(it);
  if (wasWatched) {
    watched_ = windows_.empty() ? nullptr : windows_.back().get();
    watch(watched_);
  }
  if (windows_.empty()) stopTimer();
}

void CoreFxWindows::timerCallback() {
  if (watched_ && cb_.spectrum) watched_->editor().setSpectrum(cb_.spectrum());
  for (auto& w : windows_) if (w->isVisible()) w->editor().tick();
}

} // namespace core::app
