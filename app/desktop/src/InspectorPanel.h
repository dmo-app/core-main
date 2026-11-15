// The Inspector, on the left of the window (Diogo 2026-11-07: "metam o inspector do lado esquerdo, consigo ver mais
// facilmente a metadata"), folded to a thin tab like an effect card ("minimiza como os plugins/filters de lado"): what is selected (notes in the pattern editor, a clip, a track) and
// its values. Note values change at once, each change one undo step through the window's history.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"

namespace core::app {

// One line of what a track or a clip is made of (Diogo 2026-11-07: "passa a ter mais descrição para começar
// memorizar"): a fact (label, value), a sentence on what the thing is (wrapped, muted), or a section heading.
struct InspectorLine {
  enum class Kind { Fact, Note, Heading } kind = Kind::Fact;
  juce::String label, value;
  bool changed = false;  // a setting away from its default: drawn in the accent
};

// The lines, drawn one under the other; it sits in a Viewport and scrolls when they do not fit.
class InspectorFacts final : public juce::Component {
public:
  void setLines(std::vector<InspectorLine> lines, int width);
  void paint(juce::Graphics& g) override;
private:
  int heightOf(const InspectorLine& line, int width) const;
  std::vector<InspectorLine> lines_;
};

class InspectorPanel final : public juce::Component {
public:
  InspectorPanel(PatternEditor::Host& host, PatternEditor& editor);

  // Called often (the window's timer): it redraws only when the selection or its values changed.
  void update(const std::string& selectedTrack, const std::string& selectedClip);

  // Folded: a thin tab with its name down it; a click on the tab opens it again. The window sizes it.
  static constexpr int kFoldedWidth = 30;
  std::function<void(bool folded)> onFold;
  void setFolded(bool folded);
  bool folded() const { return folded_; }
  void mouseUp(const juce::MouseEvent& e) override;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  enum class Mode { Nothing, Notes, Clip, Track };
  std::vector<doc::Note> selectedNotes() const;
  void editNotes(const juce::String& label, const std::function<void(doc::Note&)>& change);
  void show();

  PatternEditor::Host& host_;
  PatternEditor& editor_;
  Mode mode_ = Mode::Nothing;
  std::string track_, clip_, seen_;
  juce::Slider pitch_, velocity_;
  juce::ComboBox length_, roll_, ramp_;  // roll: a ratchet (the note played 2..16 times in its length); ramp: its velocities
  juce::Label pitchLabel_, lengthLabel_, velocityLabel_, rollLabel_, rampLabel_;
  juce::TextButton fold_;
  bool folded_ = false;
  std::vector<InspectorLine> facts_;  // for a clip or a track
  InspectorFacts factsView_;
  juce::Viewport factsScroll_;
  int factsTop_ = 0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorPanel)
};

} // namespace core::app
