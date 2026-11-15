// The instrument editor docked on the right of the arrangement (Diogo 2026-11-09, the chosen layout): the selected
// track's own instrument (Kick Designer, W-Core Synth, the built-in sounds), open beside the tracks while the Core
// Chain stays at the bottom. ▾ minimises it to a narrow vertical bar with its name (a click opens it again), ▢ unpins
// it into a window of its own that can leave Core's window (Diogo 2026-11-09: "o quadrado é unpin"; ▣ there pins it
// back), ✕ hides it. Layout only: nothing here goes into Undo.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"

namespace core::app {

class WCoreModular;
class SoundPage;

class InstrumentDock final : public juce::Component {
public:
  explicit InstrumentDock(PatternEditor::Host& host);
  ~InstrumentDock() override;
  void setTrack(const std::string& trackId);  // follows the selected track
  const std::string& track() const { return trackId_; }
  void documentChanged();
  bool hasEditor() const;  // the track has an instrument this dock can show
  bool collapsed() const { return collapsed_; }
  void setCollapsed(bool collapsed);
  juce::String title() const;
  static constexpr int kCollapsedWidth = 30, kDefaultWidth = 560, kMinWidth = 380;

  std::function<void()> onLayoutChanged;                              // collapsed / expanded: the workspace makes room
  std::function<void()> onClose;                                      // ✕
  std::function<void(bool unpin)> onUnpin;                            // ▢ (true) / ▣ in its own window (false)
  bool unpinned() const { return unpinned_; }
  void setUnpinned(bool unpinned);                                    // the buttons follow (the host moves it)
  std::function<void(WCoreModular&)> configureWave;                   // its callbacks (library, panels, modulation)

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseUp(const juce::MouseEvent& e) override;

private:
  std::string kindOf() const;  // "wave", "builtin" or "" (no editor here)
  void rebuild();
  void refreshHeader();  // the instrument button's name, and the W-Core button where there is no editor
  PatternEditor::Host& host_;
  std::string trackId_, builtKind_, builtTrack_;
  bool collapsed_ = false, unpinned_ = false;
  void chooseInstrument(juce::Component& from);  // the track's instrument: any of Core's own (Undo puts it back)
  void useInstrument(const std::string& name);
  juce::TextButton unpin_, minimise_, close_, instrument_, useWave_;
  std::unique_ptr<WCoreModular> wave_;
  std::unique_ptr<SoundPage> sound_;
};

}  // namespace core::app
