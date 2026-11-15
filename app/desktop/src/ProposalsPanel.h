// The AI's proposals, as Core shows them (the AI button at the bottom right). The AI lives in AICORE (Diogo,
// 2026-11-05): it reads the functions and the song Core writes, and drops proposals of calls in the bridge folder
// (control/bridge.hpp). Each one is shown checked against the song: what it calls, where, its knobs, the warnings
// (types or styles that do not fit) and what was left out. Apply makes the calls as one "AI: ..." step of Undo;
// Dismiss drops it. Either way AICORE gets the result back. Nothing runs unless Diogo presses Apply.
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

class ProposalsPanel final : public juce::Component {
public:
  struct Item {
    juce::String title;              // the summary, or the file's name
    std::vector<juce::String> calls; // one line per call that can be applied
    std::vector<juce::String> warnings, problems;
    bool canApply = false;
  };

  ProposalsPanel();
  ~ProposalsPanel() override;

  void setItems(std::vector<Item> items, const juce::String& emptyText);
  std::function<void(int index, juce::Component* button)> onApply;
  std::function<void(int index)> onDismiss;
  int preferredHeight() const;

  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  class Row;
  std::vector<std::unique_ptr<Row>> rows_;
  juce::Component content_;
  juce::Viewport viewport_;
  juce::String empty_;
  // The panel opens in its own window (a call-out), which the main window's tooltips do not reach.
  juce::TooltipWindow tips_{this, 450};
};
