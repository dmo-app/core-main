// The Patterns window (P-Core, Diogo 2026-11-08: "abre sempre uma janela extra"): a window of its own, like every
// plugin, that can stay open beside the others. Laid out as Diogo's mockup:
//   top:    CORE | PATTERNS, play / stop of the pattern alone, the BPM, the pattern, undo / redo, Send to Arrange
//   left:   templates (the P-Core styles and your own), search, All / Built-in / Mine, Preview, Load Template
//   centre: the channel rack: a row per note track with its colour, M, S, name and sound, FX, volume and meter;
//           drums as 1/16 steps, the bass and leads as a small piano roll with a "Piano" button; the bars (1-8),
//           the grid, swing and Follow above, the bar tabs (BAR 1...), the playhead line
//   under:  the velocity of the chosen row; Duplicate Pattern, Clear Selected, + Track, the patterns (Main Groove,
//           Variation, Fill, +) and Save as Template
// A pattern of the window is a slot (pattern_rack.hpp): one pattern per track. Every click is one undo step, a drag
// one gesture. The row's right-click has P-Core's shapes (KBBB, KBB_...), another take and its part.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "pattern_rack.hpp"
#include "Theme.h"

namespace core::app {

class PatternsPanel final : public juce::Component, private juce::Timer {
public:
  struct Host {
    std::function<const doc::Document&()> document;
    std::function<void(const std::function<void(doc::Document&)>&, const juce::String&)> edit;  // one undo step
    std::function<void(const juce::String&)> startGesture;
    std::function<void()> finishGesture;
    std::function<void()> undo, redo;
    std::function<double()> songTick;              // the song's playhead (where Send to Arrange puts the pattern)
    // Plays a slot alone, looped (doc::rackPlayDocument); `preview`: another song to play instead (a template not
    // loaded yet), the song itself never changes. -1 from patternTick: the window is not playing.
    std::function<void(const std::string& slotId, std::optional<doc::Document> preview, int64_t openTick)> playSlot;
    std::function<void()> stopSlot;
    std::function<double()> patternTick;
    std::function<void()> beginTempo, endTempo;    // a drag of the BPM is one gesture
    std::function<void(double)> setTempo;
    std::function<void(const std::string& trackId, const std::string& patternId)> openPianoRoll;
    std::function<void(const std::string& trackId)> openEffects;
    std::function<void()> addTrack;                // the main window's + Track menu
    std::function<float(const std::string& trackId)> level;  // the meter, 0..1+
    std::function<juce::Colour(const doc::Track&)> colourOf;
    std::function<const doc::PCoreCatalog*()> catalog;
    std::function<std::string()> songStyle;        // "Hi-tech"... ("" none)
    juce::File templatesFolder;                    // the user's pattern templates
  };

  explicit PatternsPanel(Host host);
  ~PatternsPanel() override;
  void documentChanged();  // after any edit, undo or open
  void opened();           // the window was shown (again): the default pattern is the one at the playhead now
  void closing();          // the window hides: its playing stops

  void paint(juce::Graphics& g) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress& key) override;
  void togglePlay() { play_.onClick(); }  // the play button: this pattern alone, looped (GlobalKeys sends Space here)

  // For the rows and the velocity lane.
  struct Geometry { int steps = 16; int64_t stepTicks = doc::kPpq / 4; int64_t barStart = 0; };
  Geometry geometry() const;
  const doc::Pattern* patternOf(const std::string& trackId) const;
  const doc::PatternSlot* slot() const;
  void select(const std::string& trackId);
  const std::string& selected() const { return selected_; }
  void setStep(const std::string& trackId, int step, bool on);  // inside a gesture when dragging
  void setVelocity(const std::string& trackId, const std::string& noteId, double velocity);
  void rowMenu(const std::string& trackId, juce::Component* target);
  void openPianoRoll(const std::string& trackId);
  Host& host() { return host_; }
  double playheadInBar() const;  // 0..1 of the shown bar, -1 when the playhead is elsewhere
  static constexpr int kHeadW = 360;  // a row's head: colour, M, S, name, Piano, FX, volume, meter

private:
  class Row;
  class Velocity;
  class TemplateList;
  void timerCallback() override;
  void change(const juce::String& label, const std::function<void(doc::Document&)>& f);  // one undo step
  void rebuildRows();
  void refreshSlots();
  void refreshTemplates();
  void layoutBottom(juce::Rectangle<int> r);
  void chooseSlot(const std::string& slotId);
  void slotMenu();
  void saveTemplate();
  void sendToArrange();
  void preview();
  void loadTemplate();
  void say(const juce::String& text);
  const doc::PatternTemplate* chosenTemplate() const;
  std::string styleId() const;

  Host host_;
  int64_t openTick_ = 0;         // the song's playhead when the window opened (the default "Main Groove")
  std::vector<doc::PatternSlot> slots_;
  std::string slotId_, selected_, lastStyle_;
  int bar_ = 0, gridSteps_ = 16;
  bool follow_ = true, previewing_ = false, playing_ = false;
  uint64_t seed_ = 1;

  // Top.
  juce::Label title_;
  juce::TextButton play_, stop_, undo_, redo_, send_;
  juce::Slider bpm_;
  juce::ComboBox pattern_;
  // Templates.
  juce::Label templatesTitle_, caption_;
  juce::TextEditor search_;
  juce::TextButton filterAll_, filterBuiltin_, filterMine_, preview_, load_;
  int filter_ = 0;  // 0 all, 1 built-in, 2 mine
  std::vector<doc::PatternTemplate> templates_, shown_;
  std::unique_ptr<TemplateList> list_;
  // Rack.
  juce::Label rackTitle_, swingLabel_, gridLabel_;
  juce::ComboBox bars_, grid_;
  juce::Slider swing_;
  juce::ToggleButton followButton_;
  juce::OwnedArray<juce::TextButton> barTabs_;
  juce::Viewport view_;
  juce::Component rows_;
  std::vector<std::unique_ptr<Row>> rowList_;
  std::string rowShape_;
  std::unique_ptr<Velocity> velocity_;
  // Bottom.
  juce::TextButton duplicate_, clear_, addTrack_, addSlot_, saveTemplate_;
  juce::OwnedArray<juce::TextButton> slotButtons_;
  juce::Label status_;
  juce::String message_;
  juce::Rectangle<int> rackArea_, numbersArea_, statusArea_, sideArea_;
};

// The window around it: its own title bar, resizable, hidden (not destroyed) when closed.
class PatternsWindow final : public juce::DocumentWindow {
public:
  explicit PatternsWindow(PatternsPanel::Host host);
  PatternsPanel& panel() { return *panel_; }
  void show();
  void closeButtonPressed() override;

private:
  PatternsPanel* panel_ = nullptr;  // owned by the window (setContentOwned)
};

} // namespace core::app
