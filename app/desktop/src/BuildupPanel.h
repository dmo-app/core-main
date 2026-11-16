// Build-up → roll → drop (core-main#96's first PCore window; the patterns themselves moved to Pattern Core (P-Core),
// the P-Core window). One-time use, "extenso mas fácil de usar" (Diogo, 2026-11-05).
// A small window over the piano roll. Always shown: the pattern (a menu with one submenu per style; every pattern
// fits any track) or "Keep the notes there", the bars and Write / Again. Two sections open on a click: "Change"
// (more or fewer notes, random notes between two notes,
// accents, shorter notes) and "Build-up → roll → drop" (open = there is a transition: its points, the silence, what
// comes after the drop, the tempo). Write puts the notes in; Again takes the last take back and writes another (new
// random draws). Closing it leaves only normal notes: nothing stays on the track, nothing runs in real time.
#pragma once
#include <functional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "pattern_tools.hpp"

namespace core::app {

class BuildupPanel final : public juce::Component {
public:
  struct Start {
    int fromBar = 1, toBar = 4;       // 1-based, inclusive (bars 7-12)
    int key = -1;                     // the track's key (-1: drums / none)
    double bpm = 145;                 // the tempo where the bars start
    bool tempoPossible = true;        // the pattern is placed in the song (a clip): the tempo can glide
    std::vector<doc::PatternPreset> library;
  };
  // Writes a take: the settings, the bars [from, to) in pattern ticks, the final BPM (0: the tempo stays), the
  // effects of the roles up to the drop (0 none, 1 filter + reverb, 2 + delay, 3 + drive) and whether the last take
  // goes first. Returns what happened, for the line under the buttons.
  std::function<juce::String(const doc::PCoreSettings&, int64_t from, int64_t to, double tempoTo, int effects, bool again)> onWrite;

  explicit BuildupPanel(const Start& start);
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  struct Row { juce::String label; std::vector<juce::Component*> parts; std::vector<int> widths; };  // width 0: the rest
  doc::PCoreSettings settings() const;
  void write(bool again);
  // The pattern menus (Diogo: "mostra tudo, apenas organiza"): one submenu per style, every pattern usable anywhere.
  void choose(bool drop);
  void showChoices();
  void relayout(); // the sections opened or closed: the rows shown and the window's height
  int layout(bool place);  // places the rows (or only measures); returns the height

  Start start_;
  std::string templateId_ = "bass/kbbb-anchor", dropId_;  // "": keep the notes there / nothing after the drop
  juce::Label status_;
  juce::OwnedArray<juce::Label> labels_;
  juce::ComboBox speed_, low_, high_, roll_, gap_, dropBars_, accentShape_, rise_, effects_;
  juce::Slider from_, to_, density_, chaos_, accent_, gate_, buildAt_, rollAt_, tempo_;
  juce::ToggleButton tempoOn_;
  juce::Label dash_, between_, rollWord_;
  juce::TextButton pattern_, drop_, changeHead_, dropHead_, write_, again_;
  bool changeOpen_ = false, dropOpen_ = false, written_ = false;
  std::vector<Row> top_, change_, drop_rows_;
};

} // namespace core::app
