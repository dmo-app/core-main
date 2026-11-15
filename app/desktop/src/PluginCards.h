// The Plugins panel (Diogo 2026-11-08): a button on the page rail, beside the Library's, shows Core's own plugins as
// cards in the Library's place. A click opens the plugin in its window (an instrument: the chosen track's, else the
// song's first track that plays it, else a new track with it; nothing is replaced).
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "core_plugins.hpp"

namespace core::app {

class PluginCards final : public juce::Component {
public:
  std::function<void(const doc::CorePluginInfo&)> onOpen;

  PluginCards() {
    for (const auto& p : doc::corePlugins()) {
      if (!p.where.empty()) continue;  // shown elsewhere, not a window of its own
      auto c = std::make_unique<Card>(*this, p);
      list_.addAndMakeVisible(*c);
      cards_.push_back(std::move(c));
    }
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    addAndMakeVisible(view_);
  }

  void paint(juce::Graphics& g) override {
    g.setColour(theme::kPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
    g.setColour(theme::kTitle);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    g.drawText("PLUGINS", getLocalBounds().removeFromTop(kHead).reduced(14, 0), juce::Justification::centredLeft, false);
  }
  void resized() override {
    view_.setBounds(getLocalBounds().withTrimmedTop(kHead).reduced(8, 0).withTrimmedBottom(8));
    const int w = view_.getWidth() - view_.getScrollBarThickness();
    int y = 0;
    for (auto& c : cards_) { c->setBounds(0, y, w, kCardH); y += kCardH + 6; }
    list_.setSize(w, y);
  }

private:
  static constexpr int kHead = 40, kCardH = 74;
  class Card final : public juce::Component, public juce::SettableTooltipClient {
  public:
    Card(PluginCards& o, const doc::CorePluginInfo& p) : owner_(o), info_(p) {
      setMouseCursor(juce::MouseCursor::PointingHandCursor);
      setTooltip(juce::String::fromUTF8(p.about.c_str()));
    }
    void paint(juce::Graphics& g) override {
      const auto r = getLocalBounds().toFloat();
      g.setColour(isMouseOver() ? theme::kControlHover : theme::kCard);
      g.fillRoundedRectangle(r, theme::kCorner);
      g.setColour(theme::kAccent);
      g.fillRoundedRectangle(r.withWidth(3.0f), 1.5f);
      auto b = getLocalBounds().reduced(12, 8);
      g.setColour(theme::kText);
      g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
      auto title = b.removeFromTop(20);
      g.drawText(juce::String::fromUTF8(info_.shortName.c_str()), title, juce::Justification::centredLeft, false);
      g.setColour(theme::kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String::fromUTF8(info_.word.c_str()), title, juce::Justification::centredRight, false);
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawFittedText(juce::String::fromUTF8(info_.about.c_str()), b, juce::Justification::topLeft, 3, 0.9f);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override {
      if (e.mouseWasClicked() && owner_.onOpen) owner_.onOpen(info_);
    }

  private:
    PluginCards& owner_;
    const doc::CorePluginInfo& info_;
  };
  juce::Viewport view_;
  juce::Component list_;
  std::vector<std::unique_ptr<Card>> cards_;
};

}  // namespace core::app
