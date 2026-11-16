// The bottom panel (Diogo 2026-11-16, design approved with "ok"): several tracks together on the song's timeline,
// "quase como o piano roll para as pistas que decidir meter" (kick, hats, a sample, leads...), so kick, bass and
// cymbals line up by eye. It replaces the single-track STEPS / piano roll down there: the Patterns window keeps the
// channel rack. Each lane shows only what it uses: a drum track one row per sound that plays, a track with notes the
// pitches it uses (folded). Every lane is editable; the lane clicked is the active one (its velocity under the lanes,
// Edit ▾ acts on it). Notes of a looping pattern show at every repeat and editing one edits them all (song_notes.hpp).
#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"
#include "Theme.h"
#include "TrackColour.h"
#include "edit.hpp"
#include "follow.hpp"
#include "song_notes.hpp"

namespace core::app {

class MultiTrackEditor final : public juce::Component, public juce::SettableTooltipClient {
public:
  using Host = PatternEditor::Host;
  // The part of the song to show when nothing was scrolled here: [from, to) song ticks (the time selection, else the
  // arrangement's view).
  std::function<std::pair<int64_t, int64_t>()> arrangeRange;
  // Edit ▾: the window shows the note tools of this track over these song ticks, under `target`.
  std::function<void(const std::string& trackId, int64_t from, int64_t to, juce::Component& target)> onEditMenu;
  std::function<void(const std::string& trackId)> onActiveTrack;  // the lane clicked: the window selects that track

  explicit MultiTrackEditor(Host& host) : host_(host) {
    setWantsKeyboardFocus(true);
    for (auto* b : {&addSelected_, &edit_, &follow_, &zoomOut_, &zoomIn_, &foldButton_}) {
      addAndMakeVisible(*b);
      b->setColour(juce::TextButton::buttonColourId, theme::kControl);
    }
    addSelected_.setButtonText(juce::String::fromUTF8("+ Arrange selection"));
    addSelected_.setTooltip(juce::String::fromUTF8("Show every track selected in the arrangement"));
    addSelected_.onClick = [this] { for (const auto& id : host_.selectedTracks()) show(id, true); };
    edit_.setButtonText(juce::String::fromUTF8("Edit ▾"));
    edit_.setTooltip(juce::String::fromUTF8("The note tools (quantize, FOLLOW, randomize...) on the active track, over the bars shown"));
    edit_.onClick = [this] { if (onEditMenu && !active_.empty()) onEditMenu(active_, from(), to(), edit_); };
    follow_.setButtonText(juce::String::fromUTF8("Follow arrange"));
    follow_.setTooltip(juce::String::fromUTF8("Show the arrangement's time selection (or what it shows) again"));
    follow_.onClick = [this] { scrolled_ = false; scrollY_ = 0; resized(); repaint(); };
    // Zoom (Diogo 2026-11-16: "poder ampliar o máximo e editar facilmente"): from 32 bars down to one beat.
    zoomIn_.setButtonText("+");
    zoomOut_.setButtonText(juce::String::fromUTF8("\u2212"));
    zoomIn_.setTooltip(juce::String::fromUTF8("Zoom in (Ctrl+wheel), down to one beat"));
    zoomOut_.setTooltip(juce::String::fromUTF8("Zoom out (Ctrl+wheel)"));
    zoomIn_.onClick = [this] { zoomBy(0.5, rulerArea().getCentreX()); };
    foldButton_.setButtonText("Fold");
    foldButton_.setClickingTogglesState(true);
    foldButton_.setToggleState(true, juce::dontSendNotification);
    foldButton_.setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
    foldButton_.setTooltip(juce::String::fromUTF8("Fold: only the keys that play (off: every key around them)"));
    foldButton_.onClick = [this] { fold_ = foldButton_.getToggleState(); scrollY_ = 0; rowH_ = 0; repaint(); };
    zoomOut_.onClick = [this] { zoomBy(2.0, rulerArea().getCentreX()); };
  }

  // The window: after any edit, undo or open.
  void documentChanged() {
    const doc::Document& d = host_.currentDocument();
    shown_.erase(std::remove_if(shown_.begin(), shown_.end(), [&](const std::string& id) {
                   const doc::Track* t = d.findTrack(id);
                   return !t || !doc::playsPatterns(*t);
                 }), shown_.end());
    if (!initialised_ && !d.tracks.empty()) {
      initialised_ = true;
      for (const std::string& id : {doc::findKickTrack(d), doc::findBassTrack(d), named(d, "hat")}) if (!id.empty()) show(id, false);
    }
    if (std::find(shown_.begin(), shown_.end(), active_) == shown_.end()) active_ = shown_.empty() ? std::string() : shown_.front();
    selected_.clear();
    resized();
    repaint();
  }
  // The track chosen in the arrangement: shown here and made the active lane.
  void setTrack(const std::string& trackId) {
    const doc::Track* t = host_.currentDocument().findTrack(trackId);
    if (!t || !doc::playsPatterns(*t)) return;
    show(trackId, false);
    if (active_ != trackId) { active_ = trackId; selected_.clear(); }
    repaint();
  }
  const std::string& activeTrack() const { return active_; }
  const std::vector<std::string>& shownTracks() const { return shown_; }
  void playheadMoved() { repaint(lanesArea().withTrimmedLeft(kHead)); }

  void paint(juce::Graphics& g) override;
  void resized() override {
    auto top = getLocalBounds().removeFromTop(kChips).reduced(4, 3);
    edit_.setBounds(top.removeFromRight(64));
    top.removeFromRight(6);
    zoomIn_.setBounds(top.removeFromRight(24));
    top.removeFromRight(2);
    zoomOut_.setBounds(top.removeFromRight(24));
    top.removeFromRight(6);
    foldButton_.setBounds(top.removeFromRight(44));
    top.removeFromRight(6);
    follow_.setBounds(top.removeFromRight(104));
    follow_.setVisible(scrolled_);
    top.removeFromRight(4);
    addSelected_.setBounds(top.removeFromRight(130));
    chipsRight_ = top.getRight();
  }
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override { hover_ = e.getPosition(); repaint(lanesArea()); }
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  static constexpr int kChips = 28, kRuler = 34, kHead = 120, kVel = 56;
  static constexpr int64_t kBar = 4 * doc::kPpq, kStep = doc::kPpq / 4;

  struct Lane {
    std::string trackId;
    juce::Rectangle<int> box;  // header + notes
    std::vector<int> rows;     // pitches, top to bottom
    bool drums = false;
    float rowH = 8;
  };
  struct Hit {
    std::string patternId, noteId;
    int64_t start = 0, length = 0;
    int pitch = 0;
    bool rightEdge = false;
    std::string trackId;
  };

  static std::string named(const doc::Document& d, const char* word) {
    for (const auto& t : d.tracks) {
      if (!doc::playsPatterns(t)) continue;
      std::string n = t.name;
      for (auto& c : n) c = (char)std::tolower((unsigned char)c);
      if (n.find(word) != std::string::npos) return t.id;
    }
    return {};
  }
  static bool drumTrack(const doc::Track& t) {
    std::string n = t.id + " " + t.name + " " + t.instrument.name + " " + (t.instrument.plugin.is_object() ? t.instrument.plugin.value("name", std::string()) : std::string());
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    for (const char* w : {"kick", "hat", "snare", "clap", "drum", "perc", "ride", "crash", "cymbal", "909", "tom", "shaker"})
      if (n.find(w) != std::string::npos) return true;
    return false;
  }
  static int defaultPitch(const doc::Track& t) {
    std::string n = t.name;
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    if (n.find("kick") != std::string::npos) return 36;
    if (n.find("open") != std::string::npos) return 46;
    if (n.find("hat") != std::string::npos) return 42;
    if (n.find("snare") != std::string::npos || n.find("clap") != std::string::npos) return 38;
    if (n.find("ride") != std::string::npos) return 51;
    if (n.find("bass") != std::string::npos) return 36;
    return 60;
  }

  void show(const std::string& id, bool toggle) {
    auto it = std::find(shown_.begin(), shown_.end(), id);
    if (it != shown_.end()) {
      if (toggle && shown_.size() > 1) { shown_.erase(it); if (active_ == id) active_ = shown_.front(); }
    } else {
      // In the song's order, so the lanes read like the arrangement.
      shown_.push_back(id);
      const doc::Document& d = host_.currentDocument();
      auto index = [&](const std::string& t) { for (size_t i = 0; i < d.tracks.size(); ++i) if (d.tracks[i].id == t) return i; return d.tracks.size(); };
      std::sort(shown_.begin(), shown_.end(), [&](const std::string& a, const std::string& b) { return index(a) < index(b); });
      if (active_.empty()) active_ = id;
    }
    repaint();
  }

  // The grid: the finest of 1/64, 1/32, 1/16, 1/8, beat, bar whose steps are at least 14 px apart.
  int64_t snapTicks() const {
    const double px = (double)std::max(1, rulerArea().getWidth()) / (double)std::max<int64_t>(1, to() - from());
    for (int64_t s : std::initializer_list<int64_t>{(int64_t)doc::kPpq / 16, (int64_t)doc::kPpq / 8, (int64_t)doc::kPpq / 4, (int64_t)doc::kPpq / 2, (int64_t)doc::kPpq, kBar})
      if (s * px >= 14.0) return s;
    return kBar;
  }
  void zoomBy(double factor, int x) {
    const int64_t a = from(), len = to() - from();
    const int64_t at = tickAt(x, false);
    const double share = (double)(at - a) / (double)std::max<int64_t>(1, len);
    viewLen_ = std::clamp<int64_t>((int64_t)std::llround((double)len * factor), doc::kPpq, 32 * kBar);
    viewFrom_ = std::max<int64_t>(0, (at - (int64_t)(share * (double)viewLen_)) / (doc::kPpq / 4) * (doc::kPpq / 4));
    scrolled_ = true;
    resized();
    repaint();
  }
  int64_t from() const {
    if (scrolled_) return viewFrom_;
    auto r = arrangeRange ? arrangeRange() : std::pair<int64_t, int64_t>{0, 8 * kBar};
    return std::max<int64_t>(0, r.first / kBar * kBar);
  }
  int64_t to() const {
    if (scrolled_) return viewFrom_ + viewLen_;
    auto r = arrangeRange ? arrangeRange() : std::pair<int64_t, int64_t>{0, 8 * kBar};
    const int64_t a = std::max<int64_t>(0, r.first / kBar * kBar);
    const int64_t b = std::max(a + kBar, (r.second + kBar - 1) / kBar * kBar);
    return std::min(b, a + 4 * kBar);  // big enough to edit: at most 4 bars (zoom out for more)
  }
  juce::Rectangle<int> chipsArea() const { return getLocalBounds().removeFromTop(kChips); }
  juce::Rectangle<int> rulerArea() const { return getLocalBounds().withTrimmedTop(kChips).removeFromTop(kRuler).withTrimmedLeft(kHead); }
  juce::Rectangle<int> lanesArea() const { return getLocalBounds().withTrimmedTop(kChips + kRuler).withTrimmedBottom(kVel); }
  int lanesHeight_ = 0;  // all the lanes together (more than lanesArea: they scroll)
  int scrollY_ = 0;
  juce::Rectangle<int> velArea() const { return getLocalBounds().removeFromBottom(kVel).withTrimmedLeft(kHead); }
  int xOf(int64_t tick) const {
    const auto r = rulerArea();
    const int64_t a = from(), b = to();
    return r.getX() + (int)((double)(tick - a) / (double)std::max<int64_t>(1, b - a) * r.getWidth());
  }
  int64_t tickAt(int x, bool snap) const {
    const auto r = rulerArea();
    const int64_t a = from(), b = to();
    const int64_t t = a + (int64_t)((double)(x - r.getX()) / std::max(1, r.getWidth()) * (double)(b - a));
    const int64_t s = snapTicks();
    return snap ? std::max<int64_t>(0, t / s * s) : std::max<int64_t>(0, t);
  }
  std::vector<Lane> lanes() const;
  std::vector<std::pair<juce::Rectangle<int>, std::string>> chips() const;
  const Lane* laneAt(juce::Point<int> p, const std::vector<Lane>& ls) const {
    for (const auto& l : ls) if (l.box.contains(p)) return &l;
    return nullptr;
  }
  int pitchAt(const Lane& l, int y) const {
    if (l.rows.empty()) return 60;
    const int i = std::clamp((int)((y - l.box.getY() - 3) / l.rowH), 0, (int)l.rows.size() - 1);
    return l.rows[(size_t)i];
  }
  juce::Rectangle<float> noteRect(const Lane& l, int64_t start, int64_t length, int pitch) const {
    const auto it = std::find(l.rows.begin(), l.rows.end(), pitch);
    const int row = it == l.rows.end() ? 0 : (int)(it - l.rows.begin());
    const float y = (float)l.box.getY() + 3.0f + row * l.rowH;
    const float x0 = (float)xOf(start), x1 = std::max(x0 + 3.0f, (float)xOf(start + length));
    return {x0, y + 1.0f, std::max(3.0f, x1 - x0 - 1.0f), std::max(2.0f, l.rowH - 2.0f)};
  }
  std::optional<Hit> hitAt(const Lane& l, juce::Point<int> p) const;
  void setVelocityAt(juce::Point<int> p);

  Host& host_;
  juce::TextButton addSelected_, edit_, follow_, zoomIn_, zoomOut_, foldButton_;
  int chipsRight_ = 0;
  std::vector<std::string> shown_;  // the lanes, in the song's order
  std::string active_;
  bool initialised_ = false;
  bool scrolled_ = false;           // scrolled or zoomed here: no longer follows the arrangement
  int64_t viewFrom_ = 0, viewLen_ = 8 * kBar;
  std::set<std::string> selected_;  // patternId|noteId on the active track
  juce::Point<int> hover_{-1, -1};
  enum class Drag { None, Move, Resize, Velocity, Ruler } drag_ = Drag::None;
  juce::Point<int> rulerStart_;
  int64_t rulerFrom_ = 0, rulerLen_ = 0, rulerTick_ = 0;
  bool fold_ = true;   // only the keys that play (Fold, as in Ableton and Bitwig)
  float rowH_ = 0;     // the keys' height set with Alt+wheel (0: as tall as fit)
  juce::String keyName(int pitch) const;
  Hit grabbed_;
  std::string dragTrack_;
  int64_t grabOffset_ = 0;
  int64_t guideTick_ = -1;  // the dragged note's start: a guide where other lanes play then
};

}  // namespace core::app
