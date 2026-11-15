#include "ToolDock.h"
#include <algorithm>
#include "Language.h"
#include "Theme.h"

namespace core::app {

using namespace theme;

namespace {
juce::String u8(const char* t) { return juce::String::fromUTF8(t); }
}

ToolDock::ToolDock() {
  view_.setScrollBarsShown(true, true, true, true);
  view_.setScrollBarThickness(8);
  addAndMakeVisible(view_);
  fold_.setButtonText(u8("\xe2\x80\x93"));  // –
  fold_.setTooltip(u8("Minimize to a tab at the side (click the tab to open it again)"));
  fold_.onClick = [this] { setFolded(true); };
  close_.setButtonText(u8("\xe2\x9c\x95"));  // ✕
  close_.setTooltip(u8("Close"));
  close_.onClick = [this] { close(); };
  for (auto* b : {&fold_, &close_}) {
    b->setWantsKeyboardFocus(false);  // Space stays the window's (play / stop)
    addAndMakeVisible(b);
  }
  setVisible(false);
}

ToolDock::~ToolDock() { view_.setViewedComponent(nullptr, false); }

void ToolDock::show(std::unique_ptr<juce::Component> tool, const juce::String& title) {
  view_.setViewedComponent(nullptr, false);
  tool_ = std::move(tool);
  title_ = title;
  folded_ = false;
  if (tool_) {
    // Clicks on its buttons, lists and sliders never keep the keyboard: Space plays and stops (text fields still type).
    std::function<void(juce::Component&)> noKeys = [&](juce::Component& c) {
      if (dynamic_cast<juce::Button*>(&c) || dynamic_cast<juce::ComboBox*>(&c) || dynamic_cast<juce::Slider*>(&c)) c.setWantsKeyboardFocus(false);
      for (auto* child : c.getChildren()) noKeys(*child);
    };
    noKeys(*tool_);
    view_.setViewedComponent(tool_.get(), false);
  }
  changed();
}

void ToolDock::close() {
  if (!tool_) return;
  view_.setViewedComponent(nullptr, false);
  // Not deleted here: close() may come from a click inside the tool, still on the stack. Next message.
  juce::MessageManager::callAsync([old = std::shared_ptr<juce::Component>(std::move(tool_))] {});
  folded_ = false;
  changed();
}

void ToolDock::setFolded(bool folded) {
  if (folded == folded_ || !tool_) return;
  folded_ = folded;
  changed();
}

void ToolDock::changed() {
  const bool shown = tool_ != nullptr;
  setVisible(shown);
  view_.setVisible(shown && !folded_);
  fold_.setVisible(shown && !folded_);
  close_.setVisible(shown && !folded_);
  setMouseCursor(folded_ ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
  setTooltip(folded_ ? u8("Open ") + title_ : juce::String());
  if (onLayoutChanged) onLayoutChanged();
  resized();
  repaint();
}

int ToolDock::wantedWidth(int room) const {
  if (!tool_) return 0;
  if (folded_) return kFoldedWidth;
  return std::max(kFoldedWidth, std::min(tool_->getWidth() + view_.getScrollBarThickness() + 2, room / 2));
}

void ToolDock::resized() {
  if (!tool_ || folded_) return;
  auto r = getLocalBounds();
  auto head = r.removeFromTop(kHeader).reduced(4, 3);
  close_.setBounds(head.removeFromRight(24));
  head.removeFromRight(2);
  fold_.setBounds(head.removeFromRight(24));
  view_.setBounds(r.reduced(1, 0).withTrimmedBottom(1));
}

void ToolDock::mouseUp(const juce::MouseEvent& e) {
  if (folded_ && e.mouseWasClicked()) setFolded(false);
}

void ToolDock::paint(juce::Graphics& g) {
  const auto r = getLocalBounds().toFloat();
  g.setColour(kPanel);
  g.fillRoundedRectangle(r, theme::kCorner);
  g.setColour(kOutline);
  g.drawRoundedRectangle(r.reduced(0.5f), theme::kCorner, 1.0f);
  if (folded_) {  // the name down the tab, as a folded effect card
    g.saveState();
    g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi).translated(r.getWidth(), 0));
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
    g.drawText(lang::tr(title_), juce::Rectangle<float>(12, 0, r.getHeight() - 24, r.getWidth()), juce::Justification::centredLeft, true);
    g.restoreState();
    return;
  }
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold).withKerningFactor(0.06f));
  g.drawText(lang::tr(title_), getLocalBounds().removeFromTop(kHeader).withTrimmedLeft(12).withTrimmedRight(64), juce::Justification::centredLeft, true);
  g.setColour(kOutline);
  g.fillRect(1, kHeader - 1, getWidth() - 2, 1);
}

} // namespace core::app
