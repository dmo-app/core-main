// The Mixer (UI02), in the bottom panel: one strip per track and bus, then the master. Each strip has its
// inserts (the effect names, lit when on), a send to a bus, its output (the master or a bus), pan, a stereo
// meter with a dB scale, the fader and mute/solo.
// Every change goes through the domain as a history step; a fader or knob drag is one gesture.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"

namespace core::app {

class MixerPanel final : public juce::Component, private juce::AsyncUpdater {
public:
  explicit MixerPanel(PatternEditor::Host& host);
  ~MixerPanel() override;

  void documentChanged();  // strips follow the tracks; values follow undo/redo
  // Peak levels (linear, 0..1+) per track id and of the master, from the engine (30 Hz).
  void setLevels(const std::map<std::string, std::pair<float, float>>& tracks, float masterLeft, float masterRight);
  // Each strip's share of one core in % (engine.cpu: a track with its instrument and inserts; the master chain).
  void setCpu(const std::map<std::string, float>& tracks, float master);
  // The strip shown as chosen ("" = the master).
  void select(const std::string& trackId);
  const std::string& selected() const { return selected_; }

  std::function<void(const std::string& trackId)> onSelect;  // a click on a strip's name ("" = the master)
  std::function<void()> onClose;                            // back to the pattern editor

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  class Strip;
  void handleAsyncUpdate() override;  // strips are rebuilt after the click that changed the tracks has returned
  std::string shapeOf() const;        // the strips: track ids and kinds, in order

  PatternEditor::Host& host_;
  std::string selected_, shape_;
  juce::Label title_;
  juce::TextButton addBus_, close_;
  juce::Viewport view_;
  juce::Component strip_;
  std::vector<std::unique_ptr<Strip>> strips_;
};

} // namespace core::app
