// The Tools panel (Diogo 2026-11-08, back 2026-11-16 with plugins only): a button on the page rail, beside the
// Library's, shows Core's tools as cards in the Library's place (doc::corePlugins()). A click opens the tool: the
// Patterns window, or the tool's menu (where it works, then it writes) on the selected tracks. The AI calls the
// same tools by the same ids. A card whose roles have no plugin yet is grey and says which to choose.
// Compact layout (2026-11-16): one line per tool (name and kind); what it does is in its tooltip.
// One device (Diogo 2026-11-16: "isto deveria estar dentro de um plugin chamado tools"): the panel shows a single
// "Tools" device; a click on it opens or folds it, and the tools are inside it. The ids the AI calls stay the same.
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "core_plugins.hpp"
#include "roles.hpp"

namespace core::app {

class PluginCards final : public juce::Component {
public:
  std::function<void(const doc::CorePluginInfo&, juce::Component&)> onOpen;

  PluginCards() {
    for (const auto& p : doc::corePlugins()) {
      auto c = std::make_unique<Card>(*this, p);
      list_.addAndMakeVisible(*c);
      cards_.push_back(std::move(c));
    }
    device_ = std::make_unique<Device>(*this);
    list_.addAndMakeVisible(*device_);
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    addAndMakeVisible(view_);
  }

  // Which roles have a plugin on this computer (Settings > Roles): a card none of whose roles has one is grey.
  void setRoles(const doc::RoleMap& map) {
    for (auto& c : cards_) c->setMissing(map);
    repaint();
  }

  void paint(juce::Graphics& g) override {
    g.setColour(theme::kPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
    g.setColour(theme::kTitle);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    g.drawText("TOOLS", getLocalBounds().removeFromTop(kHead).reduced(10, 0), juce::Justification::centredLeft, false);
  }
  void resized() override {
    view_.setBounds(getLocalBounds().withTrimmedTop(kHead).reduced(4, 0).withTrimmedBottom(4));
    const int w = view_.getWidth() - view_.getScrollBarThickness();
    int y = 0;
    device_->setBounds(0, y, w, kCardH);
    y += kCardH + 2;
    for (auto& c : cards_) {
      c->setVisible(open_);
      if (!open_) continue;
      c->setBounds(10, y, w - 10, kCardH);  // inside the device: indented under it
      y += kCardH + 2;
    }
    list_.setSize(w, y);
  }

private:
  static constexpr int kHead = 28, kCardH = 28;
  bool open_ = false;  // folded: one device; a click shows the tools inside
  void toggleOpen() { open_ = !open_; resized(); repaint(); }
  // The device itself: its name, how many tools it holds, an arrow that says open or folded.
  class Device final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit Device(PluginCards& o) : owner_(o) {
      setMouseCursor(juce::MouseCursor::PointingHandCursor);
      setTooltip(juce::String::fromUTF8("Core's tools in one device (the AI calls the same ones). Click: open / fold."));
    }
    void paint(juce::Graphics& g) override {
      const auto r = getLocalBounds().toFloat();
      g.setColour(isMouseOver() ? theme::kControlHover : theme::kCard.brighter(0.06f));
      g.fillRoundedRectangle(r, theme::kCorner);
      g.setColour(theme::kAccent);
      g.fillRoundedRectangle(r.withWidth(3.0f), 1.5f);
      juce::Path arrow;
      const float cx = 14.0f, cy = r.getCentreY();
      if (owner_.open_) arrow.addTriangle(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
      else arrow.addTriangle(cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy);
      g.setColour(theme::kMuted);
      g.fillPath(arrow);
      const auto title = getLocalBounds().withTrimmedLeft(24).withTrimmedRight(10);
      g.setColour(theme::kText);
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      g.drawText("Tools", title, juce::Justification::centredLeft, false);
      g.setColour(theme::kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String((int)owner_.cards_.size()) + " tools", title, juce::Justification::centredRight, false);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override { if (e.mouseWasClicked()) owner_.toggleOpen(); }

  private:
    PluginCards& owner_;
  };
  class Card final : public juce::Component, public juce::SettableTooltipClient {
  public:
    Card(PluginCards& o, const doc::CorePluginInfo& p) : owner_(o), info_(p) {
      setMouseCursor(juce::MouseCursor::PointingHandCursor);
      setTooltip(juce::String::fromUTF8(p.about.c_str()));
    }
    void setMissing(const doc::RoleMap& map) {
      missing_.clear();
      bool any = info_.roles.empty();
      for (const auto& r : info_.roles) {
        if (map.find(r)) any = true;
        else missing_ += (missing_.isEmpty() ? juce::String() : juce::String(", ")) + juce::String::fromUTF8(r.c_str());
      }
      grey_ = !any;
      setTooltip(juce::String::fromUTF8(info_.about.c_str()) +
                 (missing_.isEmpty() ? juce::String() : juce::String::fromUTF8("\nSettings > Roles: choose the plugin of ") + missing_));
    }
    void paint(juce::Graphics& g) override {
      const auto r = getLocalBounds().toFloat();
      g.setColour(isMouseOver() ? theme::kControlHover : theme::kCard);
      g.fillRoundedRectangle(r, theme::kCorner);
      g.setColour(grey_ ? theme::kDim : theme::kAccent);
      g.fillRoundedRectangle(r.withWidth(3.0f), 1.5f);
      const auto title = getLocalBounds().reduced(10, 0);
      g.setColour(grey_ ? theme::kMuted : theme::kText);
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      g.drawText(juce::String::fromUTF8(info_.shortName.c_str()), title, juce::Justification::centredLeft, true);
      g.setColour(theme::kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String::fromUTF8(grey_ ? "no plugin" : info_.word.c_str()), title, juce::Justification::centredRight, false);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override {
      if (e.mouseWasClicked() && owner_.onOpen) owner_.onOpen(info_, *this);
    }

  private:
    PluginCards& owner_;
    const doc::CorePluginInfo& info_;
    juce::String missing_;
    bool grey_ = false;
  };
  juce::Viewport view_;
  juce::Component list_;
  std::vector<std::unique_ptr<Card>> cards_;
  std::unique_ptr<Device> device_;
};

}  // namespace core::app
