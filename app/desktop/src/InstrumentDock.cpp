#include "InstrumentDock.h"
#include "SoundPage.h"
#include "Theme.h"
#include "TrackColour.h"
#include "WCoreModular.h"
#include "core_plugins.hpp"
#include "edit.hpp"
#include "wcore.hpp"

namespace core::app {

namespace {
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
constexpr int kHead = 34;
}  // namespace

InstrumentDock::InstrumentDock(PatternEditor::Host& host) : host_(host) {
  unpin_.onClick = [this] {  // after the click: the host moves this dock (and its buttons) to another window
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<InstrumentDock>(this)] {
      if (safe && safe->onUnpin) safe->onUnpin(!safe->unpinned_);
    });
  };
  setUnpinned(false);
  minimise_.setButtonText(juce::String::fromUTF8("\xe2\x96\xbe"));
  minimise_.setTooltip("Minimise to the side (the instrument keeps playing)");
  minimise_.onClick = [this] {
    if (unpinned_) { if (auto* peer = getPeer()) peer->setMinimised(true); return; }  // its own window: the window minimises
    setCollapsed(true);
  };
  close_.setButtonText(juce::String::fromUTF8("\xe2\x9c\x95"));
  close_.setTooltip("Hide the instrument editor (the instrument stays on the track)");
  close_.onClick = [this] { if (onClose) onClose(); };
  // The track's instrument, in the header (Diogo 2026-11-09 "depois de selecionar o bass nao consigo associar ao synth").
  instrument_.setTooltip("The instrument that plays this track: choose another (its notes, effects and volume stay)");
  instrument_.onClick = [this] { chooseInstrument(instrument_); };
  useWave_.setButtonText("Play this track with the W-Core Synth");
  useWave_.setTooltip("The W-Core Synth plays this track's notes (its notes, effects and volume stay; Undo puts it back)");
  useWave_.onClick = [this] { useInstrument(doc::kWaveInstrument); };
  for (auto* b : {&minimise_, &unpin_, &close_, &instrument_, &useWave_}) { b->setWantsKeyboardFocus(false); addAndMakeVisible(b); }
}

void InstrumentDock::chooseInstrument(juce::Component& from) {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  if (!t || !doc::playsPatterns(*t)) return;
  juce::PopupMenu menu;
  const auto& native = doc::builtinInstruments();
  for (size_t i = 0; i < native.size(); ++i)
    menu.addItem(juce::PopupMenu::Item(u8(native[i].label)).setID((int)i + 1)
                     .setTicked(t->instrument.kind == "builtin" && t->instrument.name == native[i].name));
  juce::Component::SafePointer<InstrumentDock> safe(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&from), [safe](int r) {
    const auto& list = doc::builtinInstruments();
    if (safe && r > 0 && r <= (int)list.size()) safe->useInstrument(list[(size_t)r - 1].name);
  });
}

void InstrumentDock::useInstrument(const std::string& name) {
  const std::string target = trackId_;
  if (target.empty()) return;
  juce::String label = u8(name);
  for (const auto& c : doc::builtinInstruments()) if (name == c.name) label = u8(c.label);
  host_.applyEdit([&](doc::Document& d) { doc::replaceInstrument(d, target, name); }, "Replace instrument: " + label);
}

InstrumentDock::~InstrumentDock() {
  wave_.reset();
  sound_.reset();
}

std::string InstrumentDock::kindOf() const {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  if (!t || t->instrument.kind != "builtin") return {};
  if (t->instrument.name == doc::kWaveInstrument) return "wave";
  return doc::findInstrumentInfo(t->instrument.name) ? "builtin" : std::string();
}

// Shown for every track that plays notes: with its editor, or with the button that puts the W-Core Synth on it.
bool InstrumentDock::hasEditor() const {
  if (!kindOf().empty()) return true;
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  return t && t->instrument.kind == "builtin" && doc::playsPatterns(*t);
}

juce::String InstrumentDock::title() const {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  if (!t) return {};
  juce::String name = u8(t->instrument.name);
  if (t->instrument.name == doc::kWaveInstrument) name = "W-Core Synth";
  else if (t->instrument.name == "kick") name = "Kick Designer";
  else if (const doc::InstrumentInfo* info = doc::findInstrumentInfo(t->instrument.name)) name = u8(info->label);
  return name;
}

void InstrumentDock::setTrack(const std::string& trackId) {
  if (trackId == trackId_) return;
  trackId_ = trackId;
  rebuild();
}

void InstrumentDock::setUnpinned(bool unpinned) {
  unpinned_ = unpinned;
  unpin_.setButtonText(juce::String::fromUTF8("\xe2\x96\xa2"));  // the square keeps its drawing in both states (the brief)
  unpin_.setTooltip(unpinned ? "Dock: back on the right of the arrangement" : "Detach: its own window, which can go outside Core's window");
  if (unpinned) collapsed_ = false;
  rebuild();
}

void InstrumentDock::setCollapsed(bool collapsed) {
  if (collapsed == collapsed_) return;
  collapsed_ = collapsed;
  rebuild();
  if (onLayoutChanged) onLayoutChanged();
}

void InstrumentDock::refreshHeader() {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  const bool plays = t && doc::playsPatterns(*t);
  juce::String label;
  if (t) for (const auto& c : doc::builtinInstruments()) if (t->instrument.kind == "builtin" && t->instrument.name == c.name) label = u8(c.label);
  instrument_.setButtonText((label.isEmpty() ? juce::String("Instrument") : label) + juce::String::fromUTF8("  \xe2\x96\xbe"));
  instrument_.setVisible(!collapsed_ && plays);
  useWave_.setVisible(!collapsed_ && plays && builtKind_.empty());
}

void InstrumentDock::rebuild() {
  const std::string kind = collapsed_ ? std::string() : kindOf();
  if (kind == builtKind_ && trackId_ == builtTrack_) { refreshHeader(); resized(); repaint(); return; }
  builtKind_ = kind;
  builtTrack_ = trackId_;
  wave_.reset();
  sound_.reset();
  if (kind == "wave") {
    wave_ = std::make_unique<WCoreModular>(host_, trackId_);
    if (configureWave) configureWave(*wave_);
    addAndMakeVisible(*wave_);
  } else if (kind == "builtin") {
    sound_ = std::make_unique<SoundPage>(host_);
    sound_->setTarget(trackId_);
    sound_->onClose = [this] { juce::MessageManager::callAsync([safe = juce::Component::SafePointer<InstrumentDock>(this)] { if (safe) safe->setCollapsed(true); }); };
    addAndMakeVisible(*sound_);
  }
  // ▾ ▢ ✕ stay at hand, minimised too (the brief): the bar's ▢ and ✕ under its name; a click on the bar opens it.
  minimise_.setVisible(!collapsed_);
  refreshHeader();
  close_.setVisible(true);
  unpin_.setVisible(true);
  resized();
  repaint();
}

void InstrumentDock::documentChanged() {
  if (kindOf() != builtKind_ && !collapsed_) { builtTrack_.clear(); rebuild(); }
  else { refreshHeader(); resized(); }
  if (wave_) wave_->documentChanged();
  if (sound_) sound_->documentChanged();
  repaint();
}

void InstrumentDock::paint(juce::Graphics& g) {
  const juce::Colour ident = theme::identityOf(*this, theme::kBlueSoft);
  g.setColour(theme::kCard);
  g.fillRect(getLocalBounds());
  g.setColour(theme::kOutline);
  g.drawRect(getLocalBounds());
  if (collapsed_) {
    // A narrow bar with the instrument's name, turned on its side; a click opens it again.
    auto r = getLocalBounds();
    g.setColour(isMouseOver() ? theme::kControlHover : theme::kBlueDeep);
    g.fillRect(r.reduced(1));
    g.setColour(ident);
    g.fillRect(r.removeFromTop(3));
    theme::drawIcon(g, "left", juce::Rectangle<float>(12.0f, 12.0f).withCentre({getWidth() / 2.0f, 16.0f}));
    juce::Graphics::ScopedSaveState state(g);
    const auto c = getLocalBounds().withTrimmedTop(28).withTrimmedBottom(56).getCentre().toFloat();
    g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi, c.x, c.y));
    g.setColour(theme::kTitle);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    g.drawText(title(), juce::Rectangle<float>((float)getHeight() - 96, (float)getWidth()).withCentre(c), juce::Justification::centred);
    return;
  }
  auto head = getLocalBounds().removeFromTop(kHead);
  g.setColour(theme::kBlueDeep);
  g.fillRect(head);
  g.setColour(ident);
  g.fillRect(head.withHeight(3));
  g.setColour(theme::kOutline);
  g.drawHorizontalLine(head.getBottom() - 1, 0.0f, (float)getWidth());
  head.removeFromLeft(12);
  g.setColour(theme::kText);
  g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
  const juce::String name = title();
  g.drawText(name, head.removeFromLeft(juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontStrong, juce::Font::bold)), name) + 12),
             juce::Justification::centredLeft, false);
  if (const doc::Track* t = host_.currentDocument().findTrack(trackId_)) {
    g.setColour(theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(u8(t->name), head.withTrimmedRight(3 * 28 + (instrument_.isVisible() ? instrument_.getWidth() + 8 : 0)), juce::Justification::centredLeft, true);
  }
  if (builtKind_.empty() && !useWave_.isVisible()) {
    g.setColour(theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawFittedText(trackId_.empty() ? juce::String("Choose a track: its instrument opens here.")
                                      : juce::String("This track has no instrument editor here."),
                     getLocalBounds().withTrimmedTop(kHead).reduced(16), juce::Justification::centred, 3);
  }
}

void InstrumentDock::resized() {
  if (collapsed_) {
    auto r = getLocalBounds().reduced(3).removeFromBottom(2 * 24 + 2);
    close_.setBounds(r.removeFromBottom(24));
    r.removeFromBottom(2);
    unpin_.setBounds(r.removeFromBottom(24));
    return;
  }
  auto r = getLocalBounds();
  auto head = r.removeFromTop(kHead).reduced(6, 5);
  // ▾ ▢ ✕, the ✕ on the right edge (in its own window too: ▾ minimises the window, ▢ docks it back).
  close_.setBounds(head.removeFromRight(26));
  head.removeFromRight(2);
  unpin_.setBounds(head.removeFromRight(26));
  head.removeFromRight(2);
  minimise_.setBounds(head.removeFromRight(26));
  head.removeFromRight(8);
  if (instrument_.isVisible())
    instrument_.setBounds(head.removeFromRight(std::min(head.getWidth() / 2,
        juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontStrong)), instrument_.getButtonText()) + 24)));
  r.reduce(1, 1);
  if (useWave_.isVisible()) useWave_.setBounds(r.withSizeKeepingCentre(std::min(r.getWidth() - 32, 300), 32));
  if (wave_) wave_->setBounds(r);  // its rows scroll inside it
  if (sound_) sound_->setBounds(r);
}

void InstrumentDock::mouseUp(const juce::MouseEvent& e) {
  if (collapsed_ && getLocalBounds().contains(e.getPosition())) setCollapsed(false);
}

}  // namespace core::app
