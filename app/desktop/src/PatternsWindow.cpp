#include "PatternsWindow.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "GlobalKeys.h"
#include "Language.h"
#include "edit.hpp"

namespace core::app {
using namespace theme;

namespace {
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
std::string str(const juce::String& s) { return s.toStdString(); }

constexpr int kTopH = 56, kSideW = 262, kStatusH = 28, kBottomH = 46, kVelocityH = 150, kRackHeadH = 40, kTabsH = 34,
              kNumbersH = 22, kRowH = 46, kGap = 8;
constexpr int64_t kBar = 4 * doc::kPpq;
constexpr int kButtonsW = 152;  // a row's head, right side: Piano, FX, volume, meter

std::string instrumentLabel(const doc::Track& t) {
  // 0.7.0: the sound comes from the track's VST3 plugin; a track without one is silent and says so.
  if (t.instrument.kind == "plugin") return t.instrument.plugin.value("name", std::string("Plugin"));
  if (t.instrument.kind == "none" || t.instrument.kind.empty()) return "No instrument: FX > Replace instrument";
  // "builtin": an old Core sound kept in the song but no longer played.
  const std::string& n = t.instrument.name;
  const std::string old = " (old, silent: choose a plugin)";
  if (n == "kick") return "Core Kick" + old;
  if (n == "wave") return "W-Core Synth" + old;
  if (n == "sampler") return "Core Sampler" + old;
  if (n == "hats") return "Core Hats" + old;
  if (n == "snare") return "Core Snare" + old;
  if (n == "drums") return "Core Drums" + old;
  return "Core" + old;
}

void flat(juce::TextButton& b, bool on = false) {
  b.setClickingTogglesState(false);
  b.setToggleState(on, juce::dontSendNotification);
}

void accentButton(juce::TextButton& b) {
  b.setColour(juce::TextButton::buttonColourId, kPrimary);
  b.setColour(juce::TextButton::buttonOnColourId, kPrimaryLight);
  b.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
}
}  // namespace

// ------------------------------------------------------------------ a row of the rack

class PatternsPanel::Row final : public juce::Component, public juce::SettableTooltipClient {
public:
  Row(PatternsPanel& owner, const doc::Track& t) : owner_(owner), id(t.id) {
    for (auto* b : {&mute_, &solo_, &fx_}) addAndMakeVisible(*b);
    mute_.setButtonText("M");
    solo_.setButtonText("S");
    fx_.setButtonText("FX");
    mute_.setTooltip(u8("Mute this track"));
    solo_.setTooltip(u8("Solo: only this track (and other soloed ones) plays"));
    fx_.setTooltip(u8("This track's effects (in the main window)"));
    mute_.setColour(juce::TextButton::buttonOnColourId, theme::kBlueLight);  // a normal button: light when on
    solo_.setColour(juce::TextButton::buttonOnColourId, kPrimary);
    mute_.onClick = [this] { toggleMix(true); };
    solo_.onClick = [this] { toggleMix(false); };
    fx_.onClick = [this] { if (owner_.host().openEffects) owner_.host().openEffects(id); };
    level_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    level_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    level_.setRange(-60.0, 12.0, 0.1);
    level_.setSkewFactorFromMidPoint(-12.0);
    level_.setDoubleClickReturnValue(true, 0.0);
    level_.setColour(juce::Slider::rotarySliderFillColourId, kAccent);
    level_.onDragStart = [this] { owner_.host().startGesture(u8("Volume")); };
    level_.onDragEnd = [this] { owner_.host().finishGesture(); };
    level_.onValueChange = [this] {
      const double v = level_.getValue();
      const std::string track = id;
      owner_.host().edit([track, v](doc::Document& d) { doc::trackById(d, track).mix.gainDb = v; }, u8("Volume"));
    };
    addAndMakeVisible(level_);
    open_.setButtonText(u8("Piano"));
    open_.setTooltip(u8("Opens this pattern in the main window's piano roll"));
    open_.onClick = [this] { owner_.openPianoRoll(id); };
    addAndMakeVisible(open_);
    update(t);
  }

  const std::string id;

  void update(const doc::Track& t) {
    name_ = u8(t.name).toUpperCase();
    sound_ = u8(instrumentLabel(t));
    colour_ = owner_.host().colourOf ? owner_.host().colourOf(t) : kAccent;
    steps_ = doc::rackStepRow(t);
    mute_.setToggleState(t.mix.mute, juce::dontSendNotification);
    solo_.setToggleState(t.mix.solo, juce::dontSendNotification);
    fx_.setToggleState(!t.effects.empty(), juce::dontSendNotification);
    fx_.setButtonText(t.effects.empty() ? juce::String("FX") : "FX " + juce::String((int)t.effects.size()));
    if (!level_.isMouseButtonDown()) level_.setValue(t.mix.gainDb, juce::dontSendNotification);
    level_.setTooltip(u8("Volume: ") + juce::String(t.mix.gainDb, 1) + u8(" dB (drag up or down; double-click: 0 dB)"));
    setTooltip(steps_ ? u8("Click a step to add or remove a hit; drag to paint. Right-click the name: P-Core shapes.")
                      : u8("Double-click: open in the piano roll. Right-click the name: P-Core shapes."));
    repaint();
  }

  void setLevel(float v) {
    v = std::clamp(v, 0.0f, 1.2f);
    if (std::abs(v - level__) < 0.01f) return;
    level__ = v;
    repaint(meterArea());
  }

  void resized() override {
    auto r = getLocalBounds().reduced(0, 9);
    r.removeFromLeft(16);
    mute_.setBounds(r.removeFromLeft(24).withSizeKeepingCentre(24, 24));
    r.removeFromLeft(4);
    solo_.setBounds(r.removeFromLeft(24).withSizeKeepingCentre(24, 24));
    auto head = getLocalBounds().withWidth(kHeadW);
    auto right = head.reduced(0, 9).removeFromRight(kButtonsW);
    right.removeFromRight(14);  // the meter
    level_.setBounds(right.removeFromRight(30).withSizeKeepingCentre(28, 28));
    right.removeFromRight(6);
    fx_.setBounds(right.removeFromRight(36).withSizeKeepingCentre(36, 22));
    right.removeFromRight(6);
    open_.setBounds(right.removeFromRight(50).withSizeKeepingCentre(50, 22));  // Diogo: Piano before FX, short ("senão fica um botão enorme")
  }

  juce::Rectangle<int> grid() const { return getLocalBounds().withTrimmedLeft(kHeadW).withTrimmedRight(10).reduced(0, 5); }
  juce::Rectangle<int> meterArea() const { return {kHeadW - 22, 10, 10, getHeight() - 20}; }

  void paint(juce::Graphics& g) override {
    const bool chosen = owner_.selected() == id;
    auto card = getLocalBounds().toFloat().reduced(2.0f, 2.0f);
    g.setColour(chosen ? kCard.brighter(0.06f) : kCard);
    g.fillRoundedRectangle(card, theme::kCorner);
    if (chosen) {
      g.setColour(kPrimaryLight);
      g.drawRoundedRectangle(card, theme::kCorner, 1.4f);
    }
    g.setColour(colour_);
    g.fillRoundedRectangle(juce::Rectangle<float>(6.0f, 10.0f, 4.0f, (float)getHeight() - 20.0f), 2.0f);
    // Name and sound.
    auto text = juce::Rectangle<int>(76, 6, kHeadW - 76 - kButtonsW - 4, getHeight() - 12);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    g.drawFittedText(name_, text.removeFromTop(text.getHeight() / 2 + 2), juce::Justification::bottomLeft, 1);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawFittedText(sound_, text, juce::Justification::topLeft, 1);
    // The meter: two thin bars.
    const auto m = meterArea().toFloat();
    for (int i = 0; i < 2; ++i) {
      auto bar = m.withWidth(3.0f).translated(i * 5.0f, 0.0f);
      g.setColour(kField);
      g.fillRect(bar);
      const float h = bar.getHeight() * std::min(1.0f, level__);
      g.setColour(level__ > 1.0f ? kError : kAccent);
      g.fillRect(bar.removeFromBottom(h));
    }
    paintGrid(g);
    const double at = owner_.playheadInBar();
    if (at >= 0) {
      const auto area = grid();
      g.setColour(kAccentBright);
      g.fillRect(juce::Rectangle<float>(area.getX() + (float)(at * area.getWidth()) - 1.0f, 0.0f, 2.0f, (float)getHeight()));
    }
  }

  void paintGrid(juce::Graphics& g) {
    const auto area = grid();
    const auto geo = owner_.geometry();
    const doc::Pattern* p = owner_.patternOf(id);
    const float w = (float)area.getWidth() / (float)geo.steps;
    const int perBeat = std::max(1, geo.steps / 4);
    if (steps_) {
      for (int i = 0; i < geo.steps; ++i) {
        auto cell = juce::Rectangle<float>(area.getX() + i * w, (float)area.getY(), w, (float)area.getHeight()).reduced(2.0f, 1.0f);
        const int64_t start = geo.barStart + i * geo.stepTicks;
        const doc::Note* hit = nullptr;
        if (p)
          for (const auto& n : p->notes)
            if (n.tick >= start && n.tick < start + geo.stepTicks) { hit = &n; break; }
        const bool oddBeat = (i / perBeat) % 2 == 1;
        if (hit) {
          g.setColour(colour_.withAlpha(0.55f + 0.45f * (float)hit->velocity));
          g.fillRoundedRectangle(cell, theme::kCorner);
          g.setColour(juce::Colours::white.withAlpha(0.55f));
          g.fillRoundedRectangle(cell.removeFromBottom(4.0f).reduced(cell.getWidth() * 0.25f, 0.5f), 1.5f);
        } else {
          g.setColour(oddBeat ? kControl.brighter(0.04f) : kControl);
          g.fillRoundedRectangle(cell, theme::kCorner);
        }
      }
    } else {
      // A small piano roll: the pattern's notes in this bar, from its lowest to its highest note.
      g.setColour(kField);
      g.fillRoundedRectangle(area.toFloat(), theme::kCorner);
      g.setColour(kGrid);
      for (int i = 1; i < geo.steps; ++i)
        if (i % perBeat == 0) g.drawVerticalLine(area.getX() + (int)std::round(i * w), (float)area.getY() + 2, (float)area.getBottom() - 2);
      if (!p || p->notes.empty()) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(theme::kFontBody));
        g.drawText(u8("Empty: click a 1/16 to add a note, right-click the name for a P-Core shape, or Piano"), area.reduced(10, 0), juce::Justification::centredLeft);
        return;
      }
      int lo = 127, hi = 0;
      for (const auto& n : p->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
      lo -= 1; hi += 1;
      const float rowH = (float)(area.getHeight() - 8) / (float)std::max(1, hi - lo);
      const double tickW = (double)area.getWidth() / (double)(geo.steps * geo.stepTicks);
      g.setColour(colour_);
      for (const auto& n : p->notes) {
        if (n.tick + n.length <= geo.barStart || n.tick >= geo.barStart + kBar) continue;
        const double x0 = area.getX() + (double)(std::max(n.tick, geo.barStart) - geo.barStart) * tickW;
        const double x1 = area.getX() + (double)(std::min(n.tick + n.length, geo.barStart + kBar) - geo.barStart) * tickW;
        const float y = (float)area.getBottom() - 4.0f - (float)(n.pitch - lo) * rowH;
        g.fillRoundedRectangle(juce::Rectangle<float>((float)x0 + 1.0f, y - 2.0f, (float)std::max(3.0, x1 - x0 - 2.0), 4.0f), 1.5f);
      }
    }
  }

  void mouseDown(const juce::MouseEvent& e) override {
    owner_.select(id);
    if (e.x < kHeadW) {
      if (e.mods.isPopupMenu()) owner_.rowMenu(id, this);
      return;
    }
    if (e.mods.isPopupMenu()) {
      owner_.rowMenu(id, this);
      return;
    }
    // Bass and lead rows too (Diogo 2026-11-08, "acabar isto" to make beats here): a click puts a note on that 1/16
    // at the row's note (its most used one, else its root), a drag paints; Piano for other notes.
    const int step = stepAt(e.x);
    if (step < 0) return;
    const auto geo = owner_.geometry();
    const doc::Pattern* p = owner_.patternOf(id);
    paintOn_ = !(p && doc::rackStepOn(*p, geo.barStart + step * geo.stepTicks, geo.stepTicks));
    dragging_ = true;
    lastStep_ = step;
    owner_.host().startGesture(u8("Steps"));
    owner_.setStep(id, step, paintOn_);
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (!dragging_) return;
    const int step = stepAt(e.x);
    if (step < 0 || step == lastStep_) return;
    lastStep_ = step;
    owner_.setStep(id, step, paintOn_);
  }
  void mouseUp(const juce::MouseEvent&) override {
    if (!dragging_) return;
    dragging_ = false;
    owner_.host().finishGesture();
  }
  void mouseDoubleClick(const juce::MouseEvent& e) override {
    if (!steps_ && e.x >= kHeadW) owner_.openPianoRoll(id);
  }

private:
  int stepAt(int x) const {
    const auto area = grid();
    if (x < area.getX() || x >= area.getRight()) return -1;
    return std::clamp((int)((x - area.getX()) * owner_.geometry().steps / std::max(1, area.getWidth())), 0, owner_.geometry().steps - 1);
  }
  void toggleMix(bool mute) {
    const std::string track = id;
    const bool on = !(mute ? mute_ : solo_).getToggleState();
    owner_.host().edit([track, mute, on](doc::Document& d) {
      auto& t = doc::trackById(d, track);
      (mute ? t.mix.mute : t.mix.solo) = on;
    }, mute ? u8("Mute") : u8("Solo"));
  }

  PatternsPanel& owner_;
  juce::String name_, sound_;
  juce::Colour colour_;
  bool steps_ = true, dragging_ = false, paintOn_ = true;
  int lastStep_ = -1;
  float level__ = 0;
  juce::TextButton mute_, solo_, fx_, open_;
  juce::Slider level_;
};

// ------------------------------------------------------------------ the velocity lane

class PatternsPanel::Velocity final : public juce::Component {
public:
  explicit Velocity(PatternsPanel& owner) : owner_(owner) {}

  juce::Rectangle<int> plot() const { return getLocalBounds().withTrimmedTop(28).withTrimmedBottom(8).withTrimmedLeft(kHeadW).withTrimmedRight(10); }

  void paint(juce::Graphics& g) override {
    g.setColour(kCard);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), theme::kCorner);
    const doc::Track* t = owner_.host().document().findTrack(owner_.selected());
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    g.drawText(u8("VELOCITY"), 14, 6, 80, 18, juce::Justification::centredLeft);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(t ? u8("· ") + u8(t->name) : u8("· choose a row"), 90, 6, 300, 18, juce::Justification::centredLeft);
    const auto area = plot();
    g.setColour(kField);
    g.fillRect(area);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (int v : {0, 32, 64, 96, 127}) {
      const int y = area.getBottom() - (int)std::round(v / 127.0 * area.getHeight());
      g.setColour(kGrid);
      g.drawHorizontalLine(y, (float)area.getX(), (float)area.getRight());
      g.setColour(kDim);
      g.drawText(juce::String(v), area.getX() - 40, y - 7, 34, 14, juce::Justification::centredRight);
    }
    const auto geo = owner_.geometry();
    const float w = (float)area.getWidth() / (float)geo.steps;
    g.setColour(kGrid.brighter(0.15f));
    for (int i = 1; i < geo.steps; ++i)
      if (i % std::max(1, geo.steps / 4) == 0) g.drawVerticalLine(area.getX() + (int)std::round(i * w), (float)area.getY(), (float)area.getBottom());
    const doc::Pattern* p = t ? owner_.patternOf(t->id) : nullptr;
    if (!p) return;
    const juce::Colour c = owner_.host().colourOf ? owner_.host().colourOf(*t) : kAccent;
    const double tickW = (double)area.getWidth() / (double)kBar;
    for (const auto& n : p->notes) {
      if (n.tick < geo.barStart || n.tick >= geo.barStart + kBar) continue;
      const float x = area.getX() + (float)((n.tick - geo.barStart) * tickW);
      const float h = (float)(n.velocity * area.getHeight());
      const float bw = std::clamp(w * 0.5f, 4.0f, 22.0f);
      auto bar = juce::Rectangle<float>(x + 2.0f, area.getBottom() - h, bw, h);
      g.setColour(c.withAlpha(0.85f));
      g.fillRect(bar);
      g.setColour(juce::Colours::white.withAlpha(0.6f));
      g.fillRect(bar.removeFromTop(2.5f));
    }
    const double at = owner_.playheadInBar();
    if (at >= 0) {
      g.setColour(kAccentBright);
      g.drawVerticalLine(area.getX() + (int)std::round(at * area.getWidth()), (float)area.getY(), (float)area.getBottom());
    }
  }

  void mouseDown(const juce::MouseEvent& e) override {
    if (!plot().contains(e.getPosition())) return;
    owner_.host().startGesture(u8("Velocity"));
    dragging_ = true;
    mouseDrag(e);
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (!dragging_) return;
    const doc::Pattern* p = owner_.patternOf(owner_.selected());
    if (!p) return;
    const auto area = plot();
    const auto geo = owner_.geometry();
    const int64_t tick = geo.barStart + (int64_t)((double)(e.x - area.getX()) / std::max(1, area.getWidth()) * kBar);
    const doc::Note* best = nullptr;
    int64_t gap = geo.stepTicks;  // within a step of the mouse
    for (const auto& n : p->notes) {
      if (n.tick < geo.barStart || n.tick >= geo.barStart + kBar) continue;
      const int64_t d = std::abs(n.tick - tick + geo.stepTicks / 4);
      if (d < gap) { gap = d; best = &n; }
    }
    if (!best) return;
    const double v = std::clamp((double)(area.getBottom() - e.y) / std::max(1, area.getHeight()), 0.02, 1.0);
    owner_.setVelocity(owner_.selected(), best->id, v);
  }
  void mouseUp(const juce::MouseEvent&) override {
    if (!dragging_) return;
    dragging_ = false;
    owner_.host().finishGesture();
  }

private:
  PatternsPanel& owner_;
  bool dragging_ = false;
};

// ------------------------------------------------------------------ the templates list

class PatternsPanel::TemplateList final : public juce::ListBox, private juce::ListBoxModel {
public:
  explicit TemplateList(PatternsPanel& owner) : owner_(owner) {
    setModel(this);
    setRowHeight(62);
    setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
  }
  int getNumRows() override { return (int)owner_.shown_.size(); }
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
    if (row < 0 || row >= (int)owner_.shown_.size()) return;
    const auto& t = owner_.shown_[(size_t)row];
    auto card = juce::Rectangle<float>(0, 0, (float)width, (float)height).reduced(1.0f, 4.0f);
    g.setColour(selected ? kPrimary.withAlpha(0.55f) : kCard);
    g.fillRoundedRectangle(card, theme::kCorner);
    g.setColour(selected ? kPrimaryLight : kOutline);
    g.drawRoundedRectangle(card, theme::kCorner, selected ? 1.5f : 1.0f);
    // A small waveform mark.
    g.setColour(selected ? kText : kMuted);
    const float cx = card.getX() + 22.0f, cy = card.getCentreY();
    const float hs[] = {6, 12, 18, 10, 14, 6};
    for (int i = 0; i < 6; ++i) g.fillRoundedRectangle(cx - 8.0f + i * 3.4f, cy - hs[i] / 2, 1.8f, hs[i], 0.9f);
    auto text = card.toNearestInt().withTrimmedLeft(46).withTrimmedRight(22).reduced(0, 8);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    g.drawFittedText(u8(t.name), text.removeFromTop(text.getHeight() / 2), juce::Justification::bottomLeft, 1);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    juce::String facts;
    if (t.bpm > 0) facts << juce::String(juce::roundToInt(t.bpm)) << " BPM · ";
    facts << juce::String(doc::rackTemplateTracks(owner_.host_.document(), t)) << " tracks · " << juce::String(t.bars) << " bars";
    if (!t.builtin) facts << u8(" · mine");
    g.drawFittedText(facts, text, juce::Justification::topLeft, 1);
    g.setColour(kMuted);
    juce::Path chevron;
    const float x = card.getRight() - 16.0f;
    chevron.startNewSubPath(x - 3.0f, cy - 5.0f);
    chevron.lineTo(x + 2.0f, cy);
    chevron.lineTo(x - 3.0f, cy + 5.0f);
    g.strokePath(chevron, juce::PathStrokeType(1.6f));
  }
  void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { owner_.loadTemplate(); }
  void selectedRowsChanged(int) override { owner_.preview_.setEnabled(owner_.chosenTemplate() != nullptr); owner_.load_.setEnabled(owner_.chosenTemplate() != nullptr); }
  juce::String getTooltipForRow(int row) override {
    return row >= 0 && row < (int)owner_.shown_.size() ? u8(owner_.shown_[(size_t)row].about) : juce::String();
  }

private:
  PatternsPanel& owner_;
};

// ------------------------------------------------------------------ the panel

PatternsPanel::PatternsPanel(Host host) : host_(std::move(host)) {
  setWantsKeyboardFocus(true);
  title_.setText(u8("PATTERNS"), juce::dontSendNotification);
  title_.setColour(juce::Label::textColourId, kTitle);
  title_.setFont(juce::FontOptions(theme::kFontHeading, juce::Font::bold));
  addAndMakeVisible(title_);
  play_.getProperties().set(kIconProperty, "play");
  stop_.getProperties().set(kIconProperty, "stop");
  undo_.getProperties().set(kIconProperty, "undo");
  redo_.getProperties().set(kIconProperty, "redo");
  play_.setTooltip(u8("Play this pattern alone, looped (Space)"));
  stop_.setTooltip(u8("Stop"));
  undo_.setTooltip(u8("Undo (Ctrl+Z)"));
  redo_.setTooltip(u8("Redo (Ctrl+Y)"));
  play_.onClick = [this] {
    const bool playing = host_.patternTick && host_.patternTick() >= 0;
    previewing_ = false;
    preview_.setButtonText(u8("Preview"));
    if (playing) host_.stopSlot();
    else host_.playSlot(slotId_, std::nullopt, openTick_);
  };
  stop_.onClick = [this] { previewing_ = false; host_.stopSlot(); preview_.setButtonText(u8("Preview")); };
  undo_.onClick = [this] { host_.undo(); };
  redo_.onClick = [this] { host_.redo(); };
  for (auto* b : {&play_, &stop_, &undo_, &redo_, &send_}) addAndMakeVisible(*b);
  bpm_.setSliderStyle(juce::Slider::LinearBar);
  bpm_.setRange(40.0, 600.0, 0.1);
  bpm_.setTextValueSuffix(" BPM");
  bpm_.setTooltip(u8("The song's tempo. Drag or type a value."));
  bpm_.onDragStart = [this] { host_.beginTempo(); };
  bpm_.onDragEnd = [this] { host_.endTempo(); };
  bpm_.onValueChange = [this] { host_.setTempo(std::round(bpm_.getValue() * 10.0) / 10.0); };
  addAndMakeVisible(bpm_);
  pattern_.setTooltip(u8("The pattern shown: one pattern on each track. Rename or delete it at the end of the list."));
  pattern_.onChange = [this] {
    const int id = pattern_.getSelectedId();
    if (id >= 1 && id <= (int)slots_.size()) chooseSlot(slots_[(size_t)id - 1].id);
    else if (id > 0) slotMenu();
  };
  addAndMakeVisible(pattern_);
  send_.setButtonText(u8("Send to Arrange"));
  send_.setTooltip(u8("Puts this pattern on the timeline at the playhead's bar, on every track that has notes in it"));
  send_.onClick = [this] { sendToArrange(); };

  // Templates.
  templatesTitle_.setText(u8("TEMPLATES"), juce::dontSendNotification);
  templatesTitle_.setFont(juce::FontOptions(theme::kFontHeading, juce::Font::bold));
  addAndMakeVisible(templatesTitle_);
  search_.setTextToShowWhenEmpty(u8("Search templates"), kDim);
  search_.onTextChange = [this] { refreshTemplates(); };
  addAndMakeVisible(search_);
  filterAll_.setButtonText(u8("All"));
  filterBuiltin_.setButtonText(u8("Built-in"));
  filterMine_.setButtonText(u8("Mine"));
  filterBuiltin_.setTooltip(u8("P-Core's styles: a groove written fresh on your tracks each time"));
  filterMine_.setTooltip(u8("The ones you saved with Save as template"));
  int f = 0;
  for (auto* b : {&filterAll_, &filterBuiltin_, &filterMine_}) {
    b->onClick = [this, f] { filter_ = f; refreshTemplates(); };
    addAndMakeVisible(*b);
    ++f;
  }
  list_ = std::make_unique<TemplateList>(*this);
  addAndMakeVisible(*list_);
  preview_.setButtonText(u8("Preview"));
  preview_.setTooltip(u8("Plays the template on your tracks without changing the song"));
  preview_.onClick = [this] { preview(); };
  load_.setButtonText(u8("Load Template"));
  load_.setTooltip(u8("Writes the template into this pattern's tracks (one undo step)"));
  accentButton(load_);
  load_.onClick = [this] { loadTemplate(); };
  addAndMakeVisible(preview_);
  addAndMakeVisible(load_);
  caption_.setText(u8("Writes editable notes into your tracks"), juce::dontSendNotification);
  caption_.setColour(juce::Label::textColourId, kDim);
  caption_.setJustificationType(juce::Justification::centred);
  caption_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::italic));
  addAndMakeVisible(caption_);

  // Rack.
  rackTitle_.setText(u8("CHANNEL RACK"), juce::dontSendNotification);
  rackTitle_.setFont(juce::FontOptions(theme::kFontHeading, juce::Font::bold));
  addAndMakeVisible(rackTitle_);
  for (int b : {1, 2, 4, 8}) bars_.addItem(juce::String(b) + (b == 1 ? " Bar" : " Bars"), b);
  bars_.setTooltip(u8("The pattern's length (its notes repeat when it grows)"));
  bars_.onChange = [this] {
    const int b = bars_.getSelectedId();
    const doc::PatternSlot* s = slot();
    if (b <= 0 || !s || s->bars == b) return;
    const std::string id = slotId_;
    change(u8("Pattern length"), [id, b](doc::Document& d) { doc::setRackBars(d, id, b); });
  };
  addAndMakeVisible(bars_);
  gridLabel_.setText(u8("Grid:"), juce::dontSendNotification);
  addAndMakeVisible(gridLabel_);
  grid_.addItem("1/8", 8);
  grid_.addItem("1/16", 16);
  grid_.addItem("1/32", 32);
  grid_.setSelectedId(16, juce::dontSendNotification);
  grid_.setTooltip(u8("How many steps a bar shows"));
  grid_.onChange = [this] { gridSteps_ = std::max(8, grid_.getSelectedId()); repaint(); rows_.repaint(); if (velocity_) velocity_->repaint(); };
  addAndMakeVisible(grid_);
  swingLabel_.setText(u8("Swing"), juce::dontSendNotification);
  addAndMakeVisible(swingLabel_);
  swing_.setSliderStyle(juce::Slider::LinearHorizontal);
  swing_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 44, 20);
  swing_.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  swing_.setRange(0, 75, 1);
  swing_.setTextValueSuffix("%");
  swing_.setDoubleClickReturnValue(true, 0);
  swing_.setTooltip(u8("Every second 1/16 a little later, on every track of this pattern"));
  swing_.onDragStart = [this] { host_.startGesture(u8("Swing")); };
  swing_.onDragEnd = [this] { host_.finishGesture(); };
  swing_.onValueChange = [this] {
    const double v = swing_.getValue() / 100.0;
    const std::string id = slotId_;
    change(u8("Swing"), [id, v](doc::Document& d) { doc::setRackSwing(d, id, v); });
  };
  addAndMakeVisible(swing_);
  followButton_.setButtonText(u8("Follow"));
  followButton_.setToggleState(true, juce::dontSendNotification);
  followButton_.setTooltip(u8("While playing, the bar shown follows the playhead"));
  followButton_.onClick = [this] { follow_ = followButton_.getToggleState(); };
  addAndMakeVisible(followButton_);
  view_.setViewedComponent(&rows_, false);
  view_.setScrollBarsShown(true, false);
  addAndMakeVisible(view_);
  velocity_ = std::make_unique<Velocity>(*this);
  addAndMakeVisible(*velocity_);

  // Bottom.
  duplicate_.setButtonText(u8("Duplicate Pattern"));
  duplicate_.setTooltip(u8("A copy of this pattern, with its own notes"));
  duplicate_.onClick = [this] {
    std::string made;
    const std::string id = slotId_;
    change(u8("Duplicate pattern"), [id, &made](doc::Document& d) { made = doc::duplicateRackSlot(d, id); });
    if (!made.empty()) chooseSlot(made);
  };
  clear_.setButtonText(u8("Clear Selected"));
  clear_.setTooltip(u8("Removes the chosen row's notes from this pattern"));
  clear_.onClick = [this] {
    if (selected_.empty()) { say(u8("Choose a row first (click its name).")); return; }
    const std::string id = slotId_, track = selected_;
    change(u8("Clear row"), [id, track](doc::Document& d) { doc::clearRackSlot(d, id, {track}); });
  };
  addTrack_.setButtonText(u8("+ Track"));
  addTrack_.setTooltip(u8("A new track (the main window's track menu)"));
  addTrack_.onClick = [this] { if (host_.addTrack) host_.addTrack(); };
  addSlot_.setButtonText("+");
  addSlot_.setTooltip(u8("A new empty pattern"));
  addSlot_.onClick = [this] {
    std::string made;
    const int bars = slot() ? slot()->bars : 4;
    change(u8("New pattern"), [&made, bars](doc::Document& d) { made = doc::addRackSlot(d, "", bars); });
    if (!made.empty()) chooseSlot(made);
  };
  saveTemplate_.setButtonText(u8("Save as template"));
  saveTemplate_.setTooltip(u8("This pattern, all its rows, as a template of yours (Templates > Mine)"));
  saveTemplate_.onClick = [this] { saveTemplate(); };
  for (auto* b : {&duplicate_, &clear_, &addTrack_, &addSlot_, &saveTemplate_}) addAndMakeVisible(*b);
  status_.setColour(juce::Label::textColourId, kMuted);
  addAndMakeVisible(status_);

  opened();
  refreshTemplates();
  startTimerHz(30);
}

PatternsPanel::~PatternsPanel() { stopTimer(); }

void PatternsPanel::opened() {
  openTick_ = (int64_t)(host_.songTick ? host_.songTick() : 0.0);
  documentChanged();
  refreshTemplates();
}

void PatternsPanel::closing() {
  previewing_ = false;
  if (host_.patternTick && host_.patternTick() >= 0) host_.stopSlot();
}

PatternsPanel::Geometry PatternsPanel::geometry() const {
  Geometry g;
  g.steps = gridSteps_;
  g.stepTicks = kBar / gridSteps_;
  g.barStart = (int64_t)bar_ * kBar;
  return g;
}

const doc::PatternSlot* PatternsPanel::slot() const { return doc::findRackSlot(slots_, slotId_); }

const doc::Pattern* PatternsPanel::patternOf(const std::string& trackId) const {
  const doc::PatternSlot* s = slot();
  return s ? doc::rackPattern(host_.document(), *s, trackId) : nullptr;
}

std::string PatternsPanel::styleId() const {
  const doc::PCoreCatalog* c = host_.catalog ? host_.catalog() : nullptr;
  if (!c) return {};
  if (const doc::PCoreStyle* st = doc::pcoreStyleFor(*c, host_.songStyle ? host_.songStyle() : std::string())) return st->id;
  return lastStyle_;
}

void PatternsPanel::change(const juce::String& label, const std::function<void(doc::Document&)>& f) {
  previewing_ = false;
  preview_.setButtonText(u8("Preview"));
  const int64_t at = openTick_;
  host_.edit([at, f](doc::Document& d) {
    doc::ensureRackSlots(d, at);
    f(d);
  }, label);
}

void PatternsPanel::say(const juce::String& text) {
  message_ = text;
  repaint(statusArea_);
}

void PatternsPanel::documentChanged() {
  refreshSlots();
  rebuildRows();
  const doc::Document& d = host_.document();
  if (!bpm_.isMouseButtonDown()) bpm_.setValue(d.bpm, juce::dontSendNotification);
  if (!selected_.empty() && !d.findTrack(selected_)) selected_.clear();
  if (selected_.empty() && !rowList_.empty()) selected_ = rowList_.front()->id;
  rows_.repaint();
  if (velocity_) velocity_->repaint();
  if (list_) list_->repaint();
  repaint();
}

void PatternsPanel::refreshSlots() {
  slots_ = doc::rackSlots(host_.document(), openTick_);
  if (!doc::findRackSlot(slots_, slotId_)) slotId_ = slots_.front().id;
  const doc::PatternSlot& s = *slot();
  pattern_.clear(juce::dontSendNotification);
  for (size_t i = 0; i < slots_.size(); ++i)
    pattern_.addItem(u8("Pattern ") + juce::String((int)i + 1).paddedLeft('0', 2) + u8(" · ") + u8(slots_[i].name), (int)i + 1);
  pattern_.addSeparator();
  pattern_.addItem(u8("Rename this pattern…"), 1001);
  pattern_.addItem(u8("Delete this pattern (its notes stay on the tracks)"), 1002);
  pattern_.setItemEnabled(1002, slots_.size() > 1);
  for (size_t i = 0; i < slots_.size(); ++i)
    if (slots_[i].id == slotId_) pattern_.setSelectedId((int)i + 1, juce::dontSendNotification);
  bars_.setSelectedId(s.bars, juce::dontSendNotification);
  if (!swing_.isMouseButtonDown()) swing_.setValue(std::round(s.swing * 100.0), juce::dontSendNotification);
  bar_ = std::clamp(bar_, 0, s.bars - 1);
  // The bar tabs.
  if (barTabs_.size() != s.bars) {
    barTabs_.clear();
    for (int i = 0; i < s.bars; ++i) {
      auto* b = barTabs_.add(new juce::TextButton(u8("BAR ") + juce::String(i + 1)));
      b->setClickingTogglesState(false);
      b->onClick = [this, i] { bar_ = i; documentChanged(); };
      addAndMakeVisible(b);
    }
  }
  for (int i = 0; i < barTabs_.size(); ++i) {
    barTabs_[i]->setToggleState(i == bar_, juce::dontSendNotification);
    barTabs_[i]->setColour(juce::TextButton::buttonOnColourId, kPrimary);  // Diogo: the grey-blue for menus, the bright blue only highlights
  }
  // The pattern buttons at the bottom.
  bool same = slotButtons_.size() == (int)slots_.size();
  for (int i = 0; same && i < slotButtons_.size(); ++i)
    same = slotButtons_[i]->getProperties()["slot"].toString() == u8(slots_[(size_t)i].id) &&
           slotButtons_[i]->getButtonText() == juce::String(i + 1).paddedLeft('0', 2) + " " + u8(slots_[(size_t)i].name);
  if (!same) {
    slotButtons_.clear();
    for (size_t i = 0; i < slots_.size(); ++i) {
      auto* b = slotButtons_.add(new juce::TextButton(juce::String((int)i + 1).paddedLeft('0', 2) + " " + u8(slots_[i].name)));
      const std::string id = slots_[i].id;
      b->getProperties().set("slot", u8(id));
      b->setColour(juce::TextButton::buttonOnColourId, kPrimary);
      b->onClick = [this, id] { chooseSlot(id); };
      addAndMakeVisible(b);
    }
  }
  for (auto* b : slotButtons_) b->setToggleState(b->getProperties()["slot"].toString() == u8(slotId_), juce::dontSendNotification);
  resized();
}

void PatternsPanel::rebuildRows() {
  const doc::Document& d = host_.document();
  std::string shape;
  for (const auto& t : d.tracks) if (doc::playsPatterns(t)) shape += t.id + "|";
  if (shape != rowShape_) {
    rowShape_ = shape;
    rowList_.clear();
    for (const auto& t : d.tracks)
      if (doc::playsPatterns(t)) {
        rowList_.push_back(std::make_unique<Row>(*this, t));
        rows_.addAndMakeVisible(*rowList_.back());
      }
    resized();
  } else {
    for (auto& r : rowList_)
      if (const doc::Track* t = d.findTrack(r->id)) r->update(*t);
  }
}

void PatternsPanel::refreshTemplates() {
  templates_.clear();
  if (const doc::PCoreCatalog* c = host_.catalog ? host_.catalog() : nullptr) templates_ = doc::rackStyleTemplates(*c);
  for (auto& t : doc::loadRackTemplates(host_.templatesFolder.getFullPathName().toStdString())) templates_.push_back(t);
  const std::string keep = chosenTemplate() ? chosenTemplate()->id : std::string();
  shown_.clear();
  const juce::String q = search_.getText().trim().toLowerCase();
  for (const auto& t : templates_) {
    if ((filter_ == 1 && !t.builtin) || (filter_ == 2 && t.builtin)) continue;
    if (q.isNotEmpty() && !(u8(t.name) + " " + u8(t.style) + " " + u8(t.about)).toLowerCase().contains(q)) continue;
    shown_.push_back(t);
  }
  int f = 0;
  for (auto* b : {&filterAll_, &filterBuiltin_, &filterMine_}) {
    b->setColour(juce::TextButton::buttonOnColourId, kPrimary);
    b->setToggleState(f++ == filter_, juce::dontSendNotification);
  }
  list_->updateContent();
  int row = 0;
  for (size_t i = 0; i < shown_.size(); ++i) if (shown_[i].id == keep) row = (int)i;
  if (!shown_.empty()) list_->selectRow(row);
  preview_.setEnabled(!shown_.empty());
  load_.setEnabled(!shown_.empty());
  list_->repaint();
}

const doc::PatternTemplate* PatternsPanel::chosenTemplate() const {
  if (!list_) return nullptr;
  const int r = list_->getSelectedRow();
  return r >= 0 && r < (int)shown_.size() ? &shown_[(size_t)r] : nullptr;
}

void PatternsPanel::chooseSlot(const std::string& slotId) {
  slotId_ = slotId;
  bar_ = 0;
  documentChanged();
  if (host_.patternTick && host_.patternTick() >= 0 && !previewing_) host_.playSlot(slotId_, std::nullopt, openTick_);  // playing: the new one plays
}

void PatternsPanel::slotMenu() {
  const int id = pattern_.getSelectedId();
  refreshSlots();  // the list shows the pattern again
  const doc::PatternSlot* s = slot();
  if (!s) return;
  if (id == 1002) {
    const std::string sid = slotId_;
    change(u8("Delete pattern"), [sid](doc::Document& d) { doc::removeRackSlot(d, sid); });
    return;
  }
  if (id != 1001) return;
  auto* w = new juce::AlertWindow(lang::tr("Rename pattern"), {}, juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", u8(s->name), lang::tr("Name:"));
  w->addButton(lang::tr("Rename"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton(lang::tr("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> win(w);
  juce::Component::SafePointer<PatternsPanel> self(this);
  const std::string sid = slotId_;
  w->enterModalState(true, juce::ModalCallbackFunction::create([win, self, sid](int ok) {
    if (ok != 1 || !win || !self) return;
    const std::string name = str(win->getTextEditorContents("name").trim().substring(0, 60));
    if (!name.empty()) self->change(u8("Rename pattern"), [sid, name](doc::Document& d) { doc::renameRackSlot(d, sid, name); });
  }), true);
}

void PatternsPanel::select(const std::string& trackId) {
  if (selected_ == trackId) return;
  selected_ = trackId;
  rows_.repaint();
  if (velocity_) velocity_->repaint();
}

void PatternsPanel::setStep(const std::string& trackId, int step, bool on) {
  const auto geo = geometry();
  const int64_t start = geo.barStart + step * geo.stepTicks;
  const doc::Pattern* p = patternOf(trackId);
  const bool now = p && doc::rackStepOn(*p, start, geo.stepTicks);
  if (now == on) return;
  const std::string id = slotId_;
  const double swing = slot() ? slot()->swing : 0.0;
  const int64_t len = geo.stepTicks;
  change(on ? u8("Add step") : u8("Remove step"), [id, trackId, start, len, swing](doc::Document& d) {
    doc::Pattern& pat = doc::rackPatternFor(d, id, trackId);
    const doc::Track* t = d.findTrack(trackId);
    const int pitch = doc::rackStepPitch(*t, &pat);
    doc::toggleRackStep(pat, start, len, pitch, 0.8, swing);
  });
}

void PatternsPanel::setVelocity(const std::string& trackId, const std::string& noteId, double velocity) {
  const std::string id = slotId_;
  change(u8("Velocity"), [id, trackId, noteId, velocity](doc::Document& d) {
    doc::Pattern& p = doc::rackPatternFor(d, id, trackId);
    for (auto& n : p.notes) if (n.id == noteId) n.velocity = velocity;
  });
}

void PatternsPanel::openPianoRoll(const std::string& trackId) {
  const doc::Pattern* p = patternOf(trackId);
  std::string pid = p ? p->id : std::string();
  if (pid.empty()) {  // no pattern of this one yet: made empty, so the piano roll opens on it
    const std::string id = slotId_;
    change(u8("New pattern"), [id, trackId, &pid](doc::Document& d) { pid = doc::rackPatternFor(d, id, trackId).id; });
  }
  if (!pid.empty() && host_.openPianoRoll) host_.openPianoRoll(trackId, pid);
}

void PatternsPanel::rowMenu(const std::string& trackId, juce::Component* target) {
  const doc::Track* t = host_.document().findTrack(trackId);
  const doc::PCoreCatalog* c = host_.catalog ? host_.catalog() : nullptr;
  if (!t) return;
  const std::string part = doc::pcoreGuessPart(*t);
  const std::string style = styleId();
  juce::PopupMenu m;
  m.addSectionHeader(u8(t->name) + (part.empty() ? juce::String() : u8(" · ") + u8(part)));
  std::vector<const doc::PCoreShape*> shapes;
  if (c && !part.empty()) {
    shapes = doc::pcoreShapesFor(*c, part, style);
    juce::PopupMenu sm;
    for (size_t i = 0; i < shapes.size() && i < 200; ++i) sm.addItem(100 + (int)i, u8(shapes[i]->name));
    m.addSubMenu(u8("P-Core shape"), sm);
    const doc::PatternSlot* current = slot();
    juce::PopupMenu keys, scales;
    keys.addItem(300, u8("Auto (song's bass/key)"), true, !current || current->root.empty());
    static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int i = 0; i < 12; ++i) keys.addItem(301 + i, roots[i], true, current && current->root == roots[i]);
    scales.addItem(400, u8("Style's scale"), true, !current || current->scale.empty());
    for (size_t i = 0; i < c->scales.size(); ++i) scales.addItem(401 + (int)i, u8(c->scales[i].name), true, current && current->scale == c->scales[i].id);
    m.addSubMenu(u8("Key for P-Core"), keys);
    m.addSubMenu(u8("Scale for P-Core"), scales);
    m.addItem(2, u8("Another take (P-Core)"));
  } else {
    m.addItem(juce::PopupMenu::Item(c ? u8("P-Core: rename the track (Kick, Bass, Hats, Lead…) so it knows the part")
                                      : u8("P-Core: pcore/styles.json is missing beside Core.exe")).setEnabled(false));
  }
  m.addSeparator();
  m.addItem(3, u8("Open Piano Roll"));
  m.addItem(4, u8("Effects…"));
  m.addItem(5, u8("Clear this row"));
  juce::Component::SafePointer<PatternsPanel> self(this);
  m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMousePosition(),
                  [self, trackId, part, style, shapes](int r) {
    if (!self || r <= 0) return;
    const std::string sid = self->slotId_;
    if (r == 3) { self->openPianoRoll(trackId); return; }
    if (r == 4) { if (self->host_.openEffects) self->host_.openEffects(trackId); return; }
    if (r == 5) { self->change(u8("Clear row"), [sid, trackId](doc::Document& d) { doc::clearRackSlot(d, sid, {trackId}); }); return; }
    const doc::PCoreCatalog* cat = self->host_.catalog ? self->host_.catalog() : nullptr;
    if (!cat) return;
    if ((r >= 300 && r <= 312) || (r >= 400 && r < 401 + (int)cat->scales.size())) {
      static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      self->change(u8("P-Core key/scale"), [=](doc::Document& d) {
        auto slots = doc::rackSlots(d);
        for (auto& slot : slots) if (slot.id == sid) {
          if (r >= 400) slot.scale = r == 400 ? std::string() : cat->scales[(size_t)(r - 401)].id;
          else slot.root = r == 300 ? std::string() : roots[r - 301];
        }
        doc::setRackSlots(d, slots);
      });
      self->say(u8("P-Core will use this key and scale for the next shape; existing notes stay."));
      return;
    }
    std::string shape;
    bool vary = false;
    if (r >= 100 && r - 100 < (int)shapes.size()) shape = shapes[(size_t)r - 100]->id;
    else if (r == 2) vary = true;
    else return;
    const uint64_t seed = self->seed_++;
    doc::PCoreResult res;
    self->change(vary ? u8("P-Core: another take") : u8("P-Core shape"), [&](doc::Document& d) {
      res = doc::rackWriteShape(d, *cat, sid, trackId, part, shape, style, seed, vary);
    });
    if (res.notes > 0) {
      const doc::PCoreShape* sh = cat->shape(res.shape);
      self->say(u8("P-Core: ") + (sh ? u8(sh->name) : u8(res.shape)) + u8(", ") + juce::String(res.notes) + u8(" notes"));
    }
  });
}

double PatternsPanel::playheadInBar() const {
  const double tick = host_.patternTick ? host_.patternTick() : -1.0;
  if (tick < 0 || (int)(tick / (double)kBar) != bar_) return -1.0;
  return std::fmod(tick, (double)kBar) / (double)kBar;
}

void PatternsPanel::sendToArrange() {
  const doc::PatternSlot* s = slot();
  if (!s) return;
  const int64_t at = (int64_t)(host_.songTick ? host_.songTick() : 0.0);
  const std::string id = slotId_;
  const int bars = s->bars;  // read now: the edit rebuilds slots_ and s points into the old one (sweep ED1, use after free)
  int placed = 0;
  change(u8("Send to Arrange"), [id, at, &placed](doc::Document& d) { placed = doc::sendRackSlotToArrange(d, id, at); });
  say(placed ? u8("On the timeline from bar ") + juce::String(at / kBar + 1) + u8(", ") + juce::String(bars) + u8(" bars, ") +
                   juce::String(placed) + u8(" tracks.")
             : u8("Nothing to send: this pattern has no notes yet."));
}

void PatternsPanel::preview() {
  if (previewing_) {
    previewing_ = false;
    preview_.setButtonText(u8("Preview"));
    host_.stopSlot();
    return;
  }
  const doc::PatternTemplate* t = chosenTemplate();
  if (!t) return;
  doc::Document copy = host_.document();
  try {
    doc::ensureRackSlots(copy, openTick_);
    doc::applyRackTemplate(copy, host_.catalog ? host_.catalog() : nullptr, *t, slotId_, seed_);
  } catch (const std::exception& e) {
    say(u8("Preview: ") + u8(e.what()));
    return;
  }
  host_.playSlot(slotId_, copy, openTick_);
  previewing_ = true;
  preview_.setButtonText(u8("Stop preview"));
  say(u8("Previewing ") + u8(t->name) + u8(": the song does not change until Load Template."));
}

void PatternsPanel::loadTemplate() {
  const doc::PatternTemplate* chosen = chosenTemplate();
  if (!chosen) return;
  const doc::PatternTemplate t = *chosen;
  const bool wasPlaying = host_.patternTick && host_.patternTick() >= 0;
  const std::string id = slotId_;
  const doc::PCoreCatalog* c = host_.catalog ? host_.catalog() : nullptr;
  const uint64_t seed = seed_;  // the take heard in the preview
  int written = 0;
  change(u8("Load template: ") + u8(t.name), [&](doc::Document& d) { written = doc::applyRackTemplate(d, c, t, id, seed); });
  if (written > 0) {
    if (t.builtin) { lastStyle_ = t.style; ++seed_; }
    say(u8(t.name) + u8(" written into ") + juce::String(written) + u8(" tracks."));
    if (wasPlaying) host_.playSlot(slotId_, std::nullopt, openTick_);
  }
}

void PatternsPanel::saveTemplate() {
  const doc::PatternSlot* s = slot();
  if (!s) return;
  auto* w = new juce::AlertWindow(lang::tr("Save as template"), lang::tr("This pattern, all its rows, goes into Templates > Mine."),
                                  juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", u8(s->name), lang::tr("Name:"));
  w->addButton(lang::tr("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton(lang::tr("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> win(w);
  juce::Component::SafePointer<PatternsPanel> self(this);
  const std::string sid = slotId_;
  w->enterModalState(true, juce::ModalCallbackFunction::create([win, self, sid](int ok) {
    if (ok != 1 || !win || !self) return;
    const std::string name = str(win->getTextEditorContents("name").trim().substring(0, 60));
    if (name.empty()) return;
    try {
      doc::Document d = self->host_.document();
      doc::ensureRackSlots(d, self->openTick_);
      doc::PatternTemplate t = doc::rackTemplateFromSlot(d, sid, name);
      t.style = self->styleId();
      doc::saveRackTemplate(self->host_.templatesFolder.getFullPathName().toStdString(), t);
      self->filter_ = 2;
      self->refreshTemplates();
      self->say(u8("Saved: ") + u8(name) + u8(" (Templates > Mine)."));
    } catch (const std::exception& e) {
      self->say(u8("Not saved: ") + u8(e.what()));
    }
  }), true);
}

void PatternsPanel::timerCallback() {
  const double tick = host_.patternTick ? host_.patternTick() : -1.0;
  const bool playing = tick >= 0;
  if (playing != playing_) {
    playing_ = playing;
    play_.getProperties().set(kIconProperty, playing ? "pause" : "play");
    play_.repaint();
    if (!playing && previewing_) { previewing_ = false; preview_.setButtonText(u8("Preview")); }
    repaint(statusArea_);
  }
  if (playing && follow_ && !previewing_) {
    const int b = (int)(tick / (double)kBar);
    if (b != bar_ && slot() && b < slot()->bars) { bar_ = b; refreshSlots(); }
  }
  for (auto& r : rowList_) r->setLevel(host_.level ? host_.level(r->id) : 0.0f);
  if (playing) { rows_.repaint(); repaint(numbersArea_); if (velocity_) velocity_->repaint(); }
}

bool PatternsPanel::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::spaceKey) { togglePlay(); return true; }
  if (key == juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 0)) { host_.undo(); return true; }
  if (key == juce::KeyPress('y', juce::ModifierKeys::ctrlModifier, 0) ||
      key == juce::KeyPress('z', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0)) { host_.redo(); return true; }
  if (key == juce::KeyPress::leftKey && bar_ > 0) { --bar_; documentChanged(); return true; }
  if (key == juce::KeyPress::rightKey && slot() && bar_ + 1 < slot()->bars) { ++bar_; documentChanged(); return true; }
  return false;
}

void PatternsPanel::resized() {
  auto r = getLocalBounds();
  // Top bar.
  auto top = r.removeFromTop(kTopH).reduced(14, 10);
  top.removeFromLeft(118);  // the CORE wordmark (painted)
  top.removeFromLeft(14);
  title_.setBounds(top.removeFromLeft(110));
  play_.setBounds(top.removeFromLeft(42));
  top.removeFromLeft(6);
  stop_.setBounds(top.removeFromLeft(42));
  top.removeFromLeft(16);
  bpm_.setBounds(top.removeFromLeft(130));
  top.removeFromLeft(16);
  send_.setBounds(top.removeFromRight(160));
  top.removeFromRight(16);
  redo_.setBounds(top.removeFromRight(38));
  undo_.setBounds(top.removeFromRight(38));
  top.removeFromRight(16);
  pattern_.setBounds(top.removeFromLeft(std::min(300, top.getWidth())));

  statusArea_ = r.removeFromBottom(kStatusH);
  status_.setBounds(statusArea_.reduced(14, 0).withTrimmedLeft(260));
  r.reduce(kGap, kGap);

  // Templates.
  sideArea_ = r.removeFromLeft(kSideW);
  auto side = sideArea_.reduced(12, 10);
  templatesTitle_.setBounds(side.removeFromTop(28));
  side.removeFromTop(6);
  search_.setBounds(side.removeFromTop(30));
  side.removeFromTop(10);
  {
    auto f = side.removeFromTop(28);
    const int w = (f.getWidth() - 12) / 3;
    filterAll_.setBounds(f.removeFromLeft(w));
    f.removeFromLeft(6);
    filterBuiltin_.setBounds(f.removeFromLeft(w));
    f.removeFromLeft(6);
    filterMine_.setBounds(f);
  }
  side.removeFromTop(10);
  caption_.setBounds(side.removeFromBottom(22));
  load_.setBounds(side.removeFromBottom(40));
  side.removeFromBottom(8);
  preview_.setBounds(side.removeFromBottom(36));
  side.removeFromBottom(12);
  list_->setBounds(side);
  r.removeFromLeft(kGap);

  // Rack.
  rackArea_ = r;
  auto rack = r.reduced(10, 8);
  auto bottom = rack.removeFromBottom(kBottomH);
  layoutBottom(bottom.reduced(0, 6));
  rack.removeFromBottom(4);
  velocity_->setBounds(rack.removeFromBottom(kVelocityH));
  rack.removeFromBottom(6);
  auto head = rack.removeFromTop(kRackHeadH).reduced(0, 5);
  rackTitle_.setBounds(head.removeFromLeft(170));
  followButton_.setBounds(head.removeFromRight(86));
  head.removeFromRight(14);
  swing_.setBounds(head.removeFromRight(170));
  swingLabel_.setBounds(head.removeFromRight(50));
  head.removeFromRight(14);
  head.removeFromLeft(10);
  bars_.setBounds(head.removeFromLeft(110));
  head.removeFromLeft(14);
  gridLabel_.setBounds(head.removeFromLeft(40));
  grid_.setBounds(head.removeFromLeft(80));
  auto tabs = rack.removeFromTop(kTabsH).reduced(0, 3);
  for (auto* b : barTabs_) {
    b->setBounds(tabs.removeFromLeft(104));
    tabs.removeFromLeft(6);
  }
  numbersArea_ = rack.removeFromTop(kNumbersH);
  view_.setBounds(rack);
  const int w = view_.getMaximumVisibleWidth();
  rows_.setSize(w, (int)rowList_.size() * kRowH);
  for (size_t i = 0; i < rowList_.size(); ++i) rowList_[i]->setBounds(0, (int)i * kRowH, w, kRowH);
  velocity_->setSize(w, velocity_->getHeight());
}

void PatternsPanel::layoutBottom(juce::Rectangle<int> b) {
  duplicate_.setBounds(b.removeFromLeft(140));
  b.removeFromLeft(8);
  clear_.setBounds(b.removeFromLeft(124));
  b.removeFromLeft(8);
  addTrack_.setBounds(b.removeFromLeft(84));
  b.removeFromLeft(18);
  saveTemplate_.setBounds(b.removeFromRight(150));
  b.removeFromRight(18);
  for (auto* s : slotButtons_) {
    const int w = std::min(136, std::max(70, b.getWidth() - 48));
    if (b.getWidth() < 70 + 48) { s->setVisible(false); continue; }
    s->setVisible(true);
    s->setBounds(b.removeFromLeft(w));
    b.removeFromLeft(6);
  }
  addSlot_.setBounds(b.removeFromLeft(40));
}

void PatternsPanel::paint(juce::Graphics& g) {
  g.fillAll(kBackground);
  // Top bar.
  auto top = getLocalBounds().removeFromTop(kTopH);
  g.setColour(kPanel);
  g.fillRect(top);
  g.setColour(kOutline);
  g.drawHorizontalLine(kTopH - 1, 0.0f, (float)getWidth());
  drawWordmark(g, top.reduced(14, 14).withWidth(118).toFloat());
  g.drawVerticalLine(14 + 118 + 6, 14.0f, (float)kTopH - 14.0f);
  // Panels.
  g.setColour(kPanel);
  g.fillRoundedRectangle(sideArea_.toFloat(), theme::kCorner);
  g.fillRoundedRectangle(rackArea_.toFloat(), theme::kCorner);
  // Rule 11: a neutral window (it holds every track), so its two sections take the blue-grey scale, light first.
  g.setColour(theme::neutralScale(2, 0));
  g.drawRect(sideArea_.toFloat(), 2.0f);
  g.setColour(theme::neutralScale(2, 1));
  g.drawRect(rackArea_.toFloat(), 2.0f);
  // Step numbers, over the rows' grid.
  const auto geo = geometry();
  const int gx = view_.getX() + kHeadW, gw = view_.getMaximumVisibleWidth() - kHeadW - 10;
  if (gw > 0) {
    const float w = (float)gw / (float)geo.steps;
    const int every = geo.steps > 16 ? 2 : 1;
    g.setFont(juce::FontOptions(theme::kFontBody));
    for (int i = 0; i < geo.steps; i += every) {
      const bool beat = i % std::max(1, geo.steps / 4) == 0;
      g.setColour(beat ? kText : kMuted);
      g.drawText(juce::String(i + 1), juce::Rectangle<float>(gx + i * w, (float)numbersArea_.getY(), w * every, (float)numbersArea_.getHeight()),
                 juce::Justification::centred);
    }
  }
  // Status bar.
  g.setColour(kPanel);
  g.fillRect(statusArea_);
  g.setColour(kOutline);
  g.drawHorizontalLine(statusArea_.getY(), 0.0f, (float)getWidth());
  auto s = statusArea_.reduced(14, 0);
  g.setColour(playing_ ? kOk : kDim);
  g.fillEllipse(s.removeFromLeft(10).withSizeKeepingCentre(8, 8).toFloat());
  s.removeFromLeft(8);
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontBody));
  g.drawText(playing_ ? (previewing_ ? u8("Previewing") : u8("Playing the pattern")) : u8("Stopped"), s.removeFromLeft(130),
             juce::Justification::centredLeft);
  g.drawText(u8("|  ") + juce::String((int)rowList_.size()) + u8(" Tracks"), s.removeFromLeft(110), juce::Justification::centredLeft);
  status_.setText(message_, juce::dontSendNotification);
}

// ------------------------------------------------------------------ the window

PatternsWindow::PatternsWindow(PatternsPanel::Host host)
    : juce::DocumentWindow(u8("Core · Patterns"), kBackground, juce::DocumentWindow::allButtons) {
  setUsingNativeTitleBar(true);
  panel_ = new PatternsPanel(std::move(host));
  setContentOwned(panel_, false);
  GlobalKeys::ownPlayStop(*this, [p = panel_] { p->togglePlay(); });  // Space here plays this pattern alone
  setResizable(true, true);
  setResizeLimits(1140, 640, 4000, 3000);
  centreWithSize(1360, 820);
}

void PatternsWindow::show() {
  const bool wasShown = isVisible();
  setVisible(true);
  if (!wasShown) panel_->opened();
  toFront(true);
  panel_->grabKeyboardFocus();
}

void PatternsWindow::closeButtonPressed() {
  panel_->closing();
  setVisible(false);
}

} // namespace core::app
