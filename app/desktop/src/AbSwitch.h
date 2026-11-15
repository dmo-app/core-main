// A/B (Diogo 2026-11-07: "eu nao tenho A B no core?"): two versions of a track's sound. The
// letter lit is the version you hear and edit; a click on the other one keeps what you have in this one and puts
// the other one on the track (one Ctrl+Z). "Copy" puts what you hear into the other letter, to start B from A.
// The sound is the instrument, its effects and its Movement (doc::TrackSound); every panel of the same track shows
// the same A/B (W-Core, the instrument strip, the effects). The two versions live while Core is open: a
// song opened or made anew starts again on A.
#pragma once
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"
#include "Theme.h"

namespace core::app {

class AbSwitch final : public juce::Component {
public:
  explicit AbSwitch(PatternEditor::Host& host);
  // The track ("" = the master's chain). master: true when "" means the master (otherwise "" = nothing shown).
  void setTrack(const std::string& trackId, bool master = false);
  void documentChanged();  // the letters follow the track (another instrument: back to A)
  void choose(bool b);     // the same as a click on A or B (self-test)
  void copyToOther();      // the same as Copy
  bool onB() const;
  static void forgetAll(); // another song: every track back to A, nothing stored
  static constexpr int kWidth = 112;

  void resized() override;

private:
  std::string key() const;
  bool active() const;
  void refresh();
  PatternEditor::Host& host_;
  std::string trackId_;
  bool master_ = false;
  juce::TextButton a_{"A"}, b_{"B"}, copy_;
  theme::InfoDot dot_;
};

}  // namespace core::app
