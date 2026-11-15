// A question in Core's own look (Diogo 2026-11-09, "melhorar exit window"): a card over the window, the title in the
// header band (a square mark and capitals, left), the question under it, and the answers on the right of the footer.
// The answer the question leans to is the light button on the right; Enter takes it and Esc cancels. Sizes and colours
// come from Theme.h (ui-sizes.md, ui-colours.md).
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

namespace core::app {

class ChoiceDialog final : public juce::Component {
public:
  struct Choice { juce::String label; int code; bool preferred = false; };
  // Shown over `parent` until an answer is chosen; then `done(code)` runs (the cancel code for Esc and ✕).
  static void show(juce::Component& parent, const juce::String& title, const juce::String& message, std::vector<Choice> choices,
                   int cancelCode, std::function<void(int)> done);

  ChoiceDialog(const juce::String& title, const juce::String& message, std::vector<Choice> choices, int cancelCode,
               std::function<void(int)> done);
  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;
  void mouseDown(const juce::MouseEvent&) override {}  // the window behind waits for the answer
  static constexpr int kWidth = 460;

private:
  juce::String title_, message_;
  std::vector<Choice> choices_;
  int cancel_;
  std::function<void(int)> done_;
  juce::OwnedArray<juce::TextButton> buttons_;
  juce::TextButton close_;
  juce::Rectangle<int> card() const;
  int cardHeight() const;
  void finish(int code);
};

}  // namespace core::app
