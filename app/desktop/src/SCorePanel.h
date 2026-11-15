// Stage Core (S-Core) in the bottom panel: the song's sections across, the note tracks down, and in each cell whether
// that track plays in that section (a click turns it on or off, one undo step each). "Song shape" lays out the
// sections for a style; "Arrange" puts every selected track (or all of them) where its part plays by the rules;
// "Rise" puts F-Core's build-up over every build-up section on the tracks that have F-Core. A track's name opens its
// own plugin below (Bass Core, Wave Core): Stage Core › Wave Core. Domain: score.hpp.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"
#include "Theme.h"

namespace core::app {

class SCorePanel final : public juce::Component {
public:
  explicit SCorePanel(PatternEditor::Host& host);
  void documentChanged();
  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

  // The same operations the mouse does (self-test).
  void applyShape(const std::string& shapeId);
  void arrange();
  void rises();
  void toggle(const std::string& trackId, const std::string& sectionId);
  std::function<void()> onClose;
  std::function<void(const std::string& trackId)> onOpenTrack;  // the track's own plugin (W-Core), else its notes
  std::function<void(const juce::String&)> onSay;               // the status line

private:
  struct Cell { int row = -1, col = -1; };
  Cell cellAt(juce::Point<int> p) const;
  juce::Rectangle<int> grid() const;
  std::vector<std::string> rows() const;  // the note tracks, in song order
  void showShapes();

  PatternEditor::Host& host_;
  juce::Label title_;
  theme::InfoDot titleDot_, shapeDot_, arrangeDot_, riseDot_;
  juce::TextButton shape_, arrange_, rise_, close_;
  Cell hover_;
};

} // namespace core::app
