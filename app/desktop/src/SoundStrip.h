// The sound of a built-in instrument (kick, hats, drums, bass synth, sampler), under its pattern editor: one quiet
// card with a knob per control and a short name; an On/Off control (Pitched, Retrigger) is a switch you click. What each knob changes is behind its tiny grey dot
// (theme::InfoDot). Each knob drag is one history step; double-click puts it back to where it started.
// W-Core has its own window and no card here. Plugins have their own windows.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "AbSwitch.h"
#include "PatternEditor.h"
#include "Theme.h"

namespace core::app {

class SoundStrip final : public juce::Component, public juce::DragAndDropTarget, private juce::Timer {
public:
  static constexpr int kHeight = 140;  // vertical bars (Diogo 2026-11-07), with room in the section boxes (2026-11-09)
  explicit SoundStrip(PatternEditor::Host& host);
  ~SoundStrip() override;
  void setTrack(const std::string& trackId);
  void documentChanged();      // knobs follow undo, redo and automation edits
  bool shows() const;          // a built-in instrument is shown (otherwise the editor gives the room back)
  // Modulation, with the sound (Diogo 2026-11-07: "tem de estar tudo junto"). A right-click on a bar offers LFO /
  // Envelope / Steps on that control (kind 0, 1, 2); a modulator dragged from the Modulation list onto a bar moves it
  // there. Each bar a modulator moves shows, beside it, the stretch it sweeps.
  std::function<void(const std::string& param, int kind)> onModulate;
  std::function<void(const std::string& modulatorId, const std::string& param)> onModulatorDropped;
  // "Open": the sound and its modulation on a page of their own (the card under the pattern editor only).
  std::function<void(const std::string& trackId)> onOpenPage;
  int preferredWidth() const;  // what the title and the bars need side by side

  bool isInterestedInDragSource(const SourceDetails& d) override;
  void itemDragMove(const SourceDetails& d) override;
  void itemDragExit(const SourceDetails& d) override;
  void itemDropped(const SourceDetails& d) override;
  void paintOverChildren(juce::Graphics& g) override;

  void paint(juce::Graphics& g) override;
  void resized() override;

  void setParam(const std::string& param, double value);  // the same as a knob move (self-test)
  void resetSound();  // every knob back to its default: one Ctrl+Z (the Reset button)

private:
  struct Knob {
    std::string param, group;
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<theme::InfoDot> dot;
    std::unique_ptr<juce::Slider> slider;     // a vertical blue bar
    std::unique_ptr<juce::Label> value;       // its value, under the bar
    std::unique_ptr<juce::TextButton> toggle;  // an On/Off control (Pitched, Retrigger): a switch, not a knob
  };
  void rebuild();
  juce::Colour groupTone(const std::string& group) const;  // rule 11: a group's tone (Kick: frost; others: the track's scale)  // the knobs of the instrument the track has now
  void showValue(Knob& k);  // the value under a bar, in the knob's own units
  void showModulateMenu(const std::string& param);
  const Knob* barAt(juce::Point<int> p) const;  // the bar (with its name and value) under a point
  PatternEditor::Host& host_;
  std::string trackId_, builtFor_;  // builtFor_: the instrument the knobs are for
  std::vector<Knob> knobs_;
  juce::Label title_, name_;
  theme::InfoDot titleDot_;
  juce::TextButton openPage_;  // "Open": sound + modulation on their own page
  std::string dropOn_;         // the bar a modulator is being dragged over
  // Live (30 Hz while shown): each modulated bar's value now, drawn as a bright line moving on the bar.
  std::map<std::string, double> live_;
  void timerCallback() override;
  void visibilityChanged() override;
  juce::TextButton reset_;  // "Reset": back to the default sound; greyed out when it already is
  AbSwitch ab_;  // A/B of the track's sound (the knobs, its effects, its movement)
  bool dragging_ = false;
};

}  // namespace core::app
