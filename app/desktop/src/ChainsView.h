// Chains (Diogo 2026-11-09, the chosen layout): the song's tracks as columns, each with its instrument, its effects in
// the order they play and "+ Add effect", stacked vertically. It shows the instances already on the tracks; a click
// opens the same instance (the instrument on the right, an effect's editor in the Core Chain), never a new one.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"

namespace core::app {

class ChainsView final : public juce::Component {
public:
  explicit ChainsView(PatternEditor::Host& host);
  ~ChainsView() override;
  void documentChanged();
  void setSelected(const std::string& trackId);
  std::function<void(const std::string& trackId)> onSelectTrack;
  std::function<void(const std::string& trackId)> onOpenInstrument;
  std::function<void(const std::string& trackId, const std::string& key)> onOpenEffect;  // key: the Core Chain's row
  std::function<void(const std::string& trackId)> onAddEffect;
  static constexpr int kColumn = 190, kGap = 8, kRow = 30;

private:
  struct Hit { juce::Rectangle<int> r; std::string track, key; int what; };  // 0 header, 1 instrument, 2 effect, 3 add
  class Content;
  PatternEditor::Host& host_;
  std::string selected_;
  juce::Viewport view_;
  std::unique_ptr<Content> content_;
  void resized() override;
};

}  // namespace core::app
