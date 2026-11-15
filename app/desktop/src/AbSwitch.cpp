#include "AbSwitch.h"
#include <map>
#include "edit.hpp"

namespace core::app {
namespace {
juce::String u8(const char* text) { return juce::String::fromUTF8(text); }

struct Entry {
  doc::TrackSound a, b;  // the letter not lit is the one stored; the lit one is the track itself
  bool onB = false;
  std::string kind;      // the instrument both were made on (doc::trackSoundKind)
};
std::map<std::string, Entry>& store() {
  static std::map<std::string, Entry> s;
  return s;
}
}  // namespace

AbSwitch::AbSwitch(PatternEditor::Host& host) : host_(host) {
  for (auto* b : {&a_, &b_}) {
    b->setClickingTogglesState(false);
    b->setWantsKeyboardFocus(false);
    b->setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
    b->setConnectedEdges(b == &a_ ? juce::Button::ConnectedOnRight : juce::Button::ConnectedOnLeft);
    addAndMakeVisible(*b);
  }
  a_.onClick = [this] { choose(false); };
  b_.onClick = [this] { choose(true); };
  a_.setTooltip(u8("Version A of this track's sound (the instrument, its effects and its movement)."));
  b_.setTooltip(u8("Version B of this track's sound. Click the other letter to hear it: what you changed here is kept."));
  copy_.setButtonText(u8("Copy"));
  copy_.setWantsKeyboardFocus(false);
  copy_.onClick = [this] { copyToOther(); };
  addAndMakeVisible(copy_);
  dot_.setTooltip(u8("A/B, as on a plugin: two versions of the sound to compare. The lit letter is the one you hear and "
                     "change. Click the other letter to switch (each click is one Ctrl+Z). Copy puts what you hear into "
                     "the other letter. Notes, the mixer and automation are not part of it."));
  addAndMakeVisible(dot_);
  refresh();
}

std::string AbSwitch::key() const { return master_ && trackId_.empty() ? std::string("#master") : trackId_; }

bool AbSwitch::active() const {
  if (trackId_.empty()) return master_;
  return host_.currentDocument().findTrack(trackId_) != nullptr;
}

void AbSwitch::setTrack(const std::string& trackId, bool master) {
  trackId_ = trackId;
  master_ = master;
  refresh();
}

void AbSwitch::documentChanged() { refresh(); }

bool AbSwitch::onB() const {
  const auto it = store().find(key());
  return it != store().end() && it->second.onB;
}

void AbSwitch::refresh() {
  const bool on = active();
  if (on) {  // another instrument on the track since: the stored letter means nothing now
    const auto it = store().find(key());
    if (it != store().end()) {
      try {
        const doc::TrackSound now = doc::trackSoundOf(host_.currentDocument(), trackId_);
        Entry& e = it->second;
        if (e.kind != doc::trackSoundKind(now, trackId_)) store().erase(it);
        // Ctrl+Z over a switch: the track has the other letter's sound again, so that letter is lit (and nothing
        // you had is lost: both stay stored).
        else if (doc::sameTrackSound(now, e.onB ? e.a : e.b) && !doc::sameTrackSound(now, e.onB ? e.b : e.a)) e.onB = !e.onB;
      } catch (const std::exception&) { store().erase(it); }
    }
  }
  const bool b = on && onB();
  a_.setToggleState(on && !b, juce::dontSendNotification);
  b_.setToggleState(b, juce::dontSendNotification);
  copy_.setTooltip(b ? u8("Copy B into A: A becomes what you hear now.") : u8("Copy A into B: B becomes what you hear now."));
  copy_.setButtonText(b ? u8("B > A") : u8("A > B"));
  for (auto* c : std::initializer_list<juce::Component*>{&a_, &b_, &copy_}) c->setEnabled(on);
}

void AbSwitch::choose(bool b) {
  if (!active()) return;
  const doc::Document& d = host_.currentDocument();
  doc::TrackSound now;
  try { now = doc::trackSoundOf(d, trackId_); } catch (const std::exception&) { return; }
  const std::string kind = doc::trackSoundKind(now, trackId_);
  auto [it, fresh] = store().try_emplace(key());
  Entry& e = it->second;
  if (fresh || e.kind != kind) e = Entry{now, now, false, kind};  // both letters start as what you hear
  if (e.onB == b) { refresh(); return; }
  (e.onB ? e.b : e.a) = now;  // what you have in this letter is kept
  const doc::TrackSound next = b ? e.b : e.a;
  e.onB = b;
  if (!doc::sameTrackSound(now, next)) {
    const std::string id = trackId_;
    host_.applyEdit([&](doc::Document& doc) { doc::setTrackSound(doc, id, next); }, b ? u8("A/B: B") : u8("A/B: A"));
  }
  refresh();
}

void AbSwitch::copyToOther() {
  if (!active()) return;
  doc::TrackSound now;
  try { now = doc::trackSoundOf(host_.currentDocument(), trackId_); } catch (const std::exception&) { return; }
  const std::string kind = doc::trackSoundKind(now, trackId_);
  auto [it, fresh] = store().try_emplace(key());
  Entry& e = it->second;
  if (fresh || e.kind != kind) e = Entry{now, now, false, kind};
  (e.onB ? e.a : e.b) = now;
  refresh();
}

void AbSwitch::forgetAll() { store().clear(); }

void AbSwitch::resized() {
  auto r = getLocalBounds();
  dot_.setBounds(r.removeFromRight(theme::InfoDot::kSlot).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
  r.removeFromRight(2);
  copy_.setBounds(r.removeFromRight(44));
  r.removeFromRight(4);
  const int w = r.getWidth() / 2;
  a_.setBounds(r.removeFromLeft(w));
  b_.setBounds(r);
}

}  // namespace core::app
