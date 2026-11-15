// The piano roll's tools (P-Core, Build-up, Attachments) docked at the right of the editor, in the window's bottom
// half, instead of a floating call-out (Diogo, 2026-11-07: "se quiser meter play e alterar o plugin fecha pq space nao
// começa o play ... deviamos meter plugin como os outros sempre aberto em baixo e minimizar ou fechar depois de
// usado"). The call-out closed at the first click outside it (the Play button included) and kept the keyboard, so
// Space did not play. Docked, it stays open while the song plays, Space reaches the window, and its header has
// minimize (a thin tab at the side, like a folded effect card or the Inspector; a click on the tab opens it again)
// and close.
#pragma once
#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

namespace core::app {

class ToolDock final : public juce::Component, public juce::SettableTooltipClient {
public:
  static constexpr int kHeader = 28, kFoldedWidth = 30;
  ToolDock();
  ~ToolDock() override;
  // Shows `tool` (its own size is its width and preferred height; taller than the room: it scrolls). Replaces the one there.
  void show(std::unique_ptr<juce::Component> tool, const juce::String& title);
  void close();
  bool hasTool() const { return tool_ != nullptr; }
  juce::Component* tool() const { return tool_.get(); }
  bool folded() const { return folded_; }
  void setFolded(bool folded);
  // The width it wants in an editor `room` px wide: the tool's width (at most half the room), the tab when folded.
  int wantedWidth(int room) const;
  std::function<void()> onLayoutChanged;  // shown, folded, opened again or closed: the editor makes room

  void resized() override;
  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& e) override;

private:
  std::unique_ptr<juce::Component> tool_;
  juce::String title_;
  bool folded_ = false;
  juce::Viewport view_;
  juce::TextButton fold_, close_;
  void changed();
};

} // namespace core::app
