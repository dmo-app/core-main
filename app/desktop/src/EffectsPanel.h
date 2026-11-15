// The Core Chain (Core Studio brief): the effect chain of one track, or of the master, in the bottom panel. On the
// left the SIGNAL PATH (the instrument, then each effect in the order it plays, + Add effect, the output); on the
// right the card of the effect chosen there, with its parameters, on/off (bypass), order and remove. Every change is
// a history step; a slider drag is one gesture. The Mixer's insert editor (knobs) shows every card in a grid instead.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "AbSwitch.h"
#include "PatternEditor.h"
#include "Theme.h"
#include "fx_racks.hpp"
#include "fcore.hpp"
#include "samples.hpp"

namespace core::app {

class EffectsPanel final : public juce::Component, private juce::AsyncUpdater {
public:
  explicit EffectsPanel(PatternEditor::Host& host);
  ~EffectsPanel() override;
  // trackId "" = the master chain.
  void setTarget(const std::string& trackId);
  const std::string& target() const { return trackId_; }
  void documentChanged();  // cards follow the chain; values follow undo/redo

  void paint(juce::Graphics& g) override;
  void resized() override;

  // The same operations the mouse does (self-test).
  void addEffect(const std::string& name, int version = 0);  // 0: the newest
  void addFilter(int type);  // a Filter of that type (Filter's mode), at the end; false past kMaxFilters
  static constexpr int kMaxFilters = 8;  // filters on one chain (Diogo 2026-11-07: "até ao 8 no máximo")
  // Opens the automation of a parameter of this chain's target (volume, or "fx:<id>:<param>").
  std::function<void(const std::string& trackId, const std::string& param)> onAutomate;
  // Attachments (Diogo 2026-11-07: "poder meter os settings dos attachments nos filters"): Start, Middle and End of
  // the chosen clip, opened beside the cards. Called again: closed.
  std::function<void(const std::string& trackId)> onAttachments;
  void setAttachmentsOpen(bool open) { attach_.setToggleState(open, juce::dontSendNotification); }
  // Core FX and Core EQ open in their own windows (CoreFxWindow.h): their cards show Open, not their values.
  std::function<void(const std::string& trackId, const std::string& effectId)> onOpenCoreFx;
  std::function<void()> onClose;  // the ✕: back to the pattern editor
  std::function<void()> onMinimise;  // ▾: the whole Core Chain to its header, and back (the host's height)
  void setCollapsed(bool collapsed);
  // The instrument row of the SIGNAL PATH: opens the track's instrument (its sound page or its own window).
  std::function<void(const std::string& trackId)> onOpenInstrument;
  void select(const std::string& key);  // a SIGNAL PATH row: an effect's id ("#<place>" without one) or "fcore"
  // The Core Chain's editors (Diogo 2026-11-09, the chosen layout): several open side by side; each one minimises to
  // a tab on the right edge (▾), unpins into a window of its own that can leave Core's window (▢, "o quadrado é
  // unpin"; ▣ there pins it back) or closes (✕). Closing only closes the view; the effect stays in the chain. Layout
  // only: none of this goes into Undo.
  void minimiseEditor(const std::string& key, bool minimised);
  void unpinEditor(const std::string& key);
  void pinEditor(const std::string& trackId, const std::string& key);  // back in the chain, beside the others
  void closeEditor(const std::string& key);
  int openEditors() const;       // open and not minimised
  void showPicker() { setPickerOpen(true); }  // + Add effect (from Chains)
  int minimisedEditors() const;
  int unpinnedEditors() const;   // this track's, in their own windows
  void setParam(int index, const std::string& param, double value);
  // Style FX racks (docs/contracts/core-fx-rack.md): «Style FX ▾» puts several of Core's own effects on the selected
  // tracks at once, set for the style chosen in the dropdown. Unset: no racks here (the master and the Master page's chain).
  std::function<std::vector<doc::RackEntry>()> racks;
  std::function<std::string()> songStyle;                          // the style of the song's template ("" if none)
  std::function<void()> onOpenRacksFolder;
  // FCore (docs/contracts/core-fcore.md): effects by intent. «FCore ▾» puts it on the track: the sampler (moves with
  // each note, on a sampler track) or the effect (moves over regions, any track). One card: intent, style, a sentence
  // that says what it does, four knobs. Shown on tracks when the racks are (wireRacks sets these).
  std::function<const doc::SampleIndex*()> sampleLibrary;            // the scanned library (the sampler's Sample ▾)
  std::function<void(const std::string& trackId)> onPrintFCore;     // Print to sample: the track over the loop region
  bool applyFCore(const std::string& mode);  // the same as the menu (self-test); false if refused
  // Values as rotary knobs in a grid (the Mixer's insert editor, ref-3) instead of rows of bars; a
  // compressor shows its transfer curve over them. Set before the first documentChanged().
  bool knobs = false;

private:
  class Card;
  class FCoreCard;
  class ChainList;
  bool chain() const { return !knobs; }  // the Core Chain layout (not the Mixer's knob grid)
  juce::Component* selectedCard() const;
  juce::Component* soloCard() const;  // the unpinned window's one card (nullptr: its effect left the chain)
  std::string selectedKey() const;
  // New cards are built after the click that changed the chain has returned (a card never deletes
  // itself inside its own button handler).
  void handleAsyncUpdate() override;
  class Picker;  // + Add effect: Core's effects as tiles, in the card area
  class EditorBar;  // the bar over an open editor: its name, the track, ▾ ▢ ✕
  class SideTab;    // a minimised editor: a vertical tab on the right edge
  class FloatWindow;  // an unpinned editor: its own window, with an EffectsPanel that shows only it
  void dropFloating(const std::string& trackId, const std::string& key, bool pin);
  void showEffectMenu(const std::string& key, juce::Component& at);  // the SIGNAL PATH's right-click
  std::string solo_;  // in an unpinned editor's window: the one editor shown ("" = the whole Core Chain)
  std::function<void(bool pin)> soloDone_;  // that window: pin it back (true), or its effect left the chain (false)
  std::vector<std::string>& openList();  // the open editors of this track, in the order they were opened
  void tidyEditors();                    // drops the editors whose effect left the chain
  void setPickerOpen(bool open);
  bool layoutPicker(juce::Rectangle<int> area);
  int filterCount() const;
  void showRackMenu();
  std::vector<std::string> fcoreTargets() const;  // the selected tracks, the shown one first
  void showFCoreMenu();
  void showDuckMenu();
  std::string fcoreStyleOfSong() const;  // the FCore style of the song's template (dark psycore when none)
  bool racksHere() const;  // a track (not the master) and the racks are known
  std::string currentStyle() const;
  std::string shapeOf() const;  // what the cards show: effects and their on/off, in order
  std::vector<std::string> targets() const;
  void applyNativeRack(const doc::RackDef& def);  // the rack's native replacement on the selected tracks  // the selected tracks when this one is among them, else this one

  PatternEditor::Host& host_;
  std::string trackId_, shape_;
  bool scrollToEnd_ = false;
  juce::Label title_, hint_;
  juce::TextButton add_, automation_, attach_, close_, minimise_, rackButton_, fcoreButton_, duckButton_;
  theme::InfoDot duckDot_;
  AbSwitch ab_;  // A/B of the track's sound (on the master: its chain)
  juce::ComboBox style_;
  std::set<std::string> expanded_;  // FCore's layers shown (FCoreCard::kInside, kPick)
  std::map<std::string, std::string> selected_;  // the chosen SIGNAL PATH row of each track ("" = the master)
  std::unique_ptr<ChainList> list_;
  std::unique_ptr<Picker> picker_;
  juce::Viewport listView_;
  juce::Rectangle<int> pathArea_;  // the SIGNAL PATH column, painted under the list
  std::unique_ptr<FCoreCard> fcoreCard_;
  juce::Viewport view_;
  juce::Component strip_;
  std::vector<std::unique_ptr<Card>> cards_;
  std::map<std::string, std::vector<std::string>> open_;      // each track's open editors (keys), in order
  std::map<std::string, std::set<std::string>> minimised_;    // each track's minimised editors
  std::map<std::string, std::unique_ptr<FloatWindow>> floating_;  // the unpinned editors, by track + "\n" + key
  std::vector<std::unique_ptr<EditorBar>> bars_;
  std::vector<std::unique_ptr<SideTab>> tabs_;
  juce::Rectangle<int> editorsHead_;  // "Bottom · 2 open · 1 minimised"
};

} // namespace core::app
