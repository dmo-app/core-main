#include "ProposalsPanel.h"
#include "Language.h"
#include "Theme.h"
#include <cmath>

namespace theme = core::app::theme;

namespace {
constexpr int kWidth = 460, kLine = 18, kPad = 10, kButtons = 28, kMaxHeight = 520;
}

// One proposal: a short title with its dots, one line of counts, then Apply and Dismiss. The words sit behind the
// dots (rule for all of Core, Diogo 2026-11-05): the grey dot holds the calls, the amber one the warnings and what was
// left out.
class ProposalsPanel::Row final : public juce::Component {
public:
  Row(ProposalsPanel& owner, int index, const Item& item) : owner_(owner), index_(index), item_(item) {
    juce::StringArray what;
    for (const auto& c : item.calls) what.add(juce::String::fromUTF8("\xE2\x80\xA2 ") + c);
    what.add(item.canApply ? juce::String("Apply makes these calls as one step of Undo.") : juce::String("Nothing here can be applied."));
    calls_.setTooltip(what.joinIntoString("\n"));
    addAndMakeVisible(calls_);
    juce::StringArray notes;
    for (const auto& w : item.warnings) notes.add(juce::String::fromUTF8("\xE2\x9A\xA0 ") + w);
    for (const auto& p : item.problems) notes.add(juce::String::fromUTF8("Left out: ") + p);
    notes_.setTooltip(notes.joinIntoString("\n"));
    addChildComponent(notes_);
    notes_.setVisible(!notes.isEmpty());

    apply_.setButtonText("Apply");
    apply_.setEnabled(item.canApply);
    apply_.onClick = [this] { if (owner_.onApply) owner_.onApply(index_, &apply_); };
    dismiss_.setButtonText("Dismiss");
    dismiss_.onClick = [this] { if (owner_.onDismiss) owner_.onDismiss(index_); };
    for (auto* b : {&apply_, &dismiss_}) { b->setWantsKeyboardFocus(false); addAndMakeVisible(b); }
  }

  int preferredHeight() const { return kPad + 2 * kLine + 6 + kButtons + kPad; }

  void paint(juce::Graphics& g) override {
    g.setColour(theme::kCard);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), theme::kCorner);
    g.setColour(theme::kText);
    g.setFont(titleFont());
    g.drawText(item_.title, titleArea(), juce::Justification::centredLeft, true);
    juce::StringArray counts;
    counts.add(juce::String((int)item_.calls.size()) + (item_.calls.size() == 1 ? " call" : " calls"));
    if (!item_.warnings.empty()) counts.add(juce::String((int)item_.warnings.size()) + (item_.warnings.size() == 1 ? " warning" : " warnings"));
    if (!item_.problems.empty()) counts.add(juce::String((int)item_.problems.size()) + " left out");
    g.setColour(theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(core::app::lang::tr(counts.joinIntoString(juce::String::fromUTF8(" \xC2\xB7 "))),
               getLocalBounds().reduced(kPad, kPad).withTrimmedTop(kLine).withHeight(kLine), juce::Justification::centredLeft, true);
  }

  void resized() override {
    // The dots sit right after the title's last letter.
    auto t = titleArea();
    const int textW = std::min(t.getWidth() - 2 * theme::InfoDot::kSlot,
                               (int)std::ceil(juce::GlyphArrangement::getStringWidth(juce::Font(titleFont()), item_.title)));
    int x = t.getX() + textW + 3;
    const int y = t.getCentreY() - theme::InfoDot::kSlot / 2;
    calls_.setBounds(x, y, theme::InfoDot::kSlot, theme::InfoDot::kSlot);
    notes_.setBounds(x + theme::InfoDot::kSlot, y, theme::InfoDot::kSlot, theme::InfoDot::kSlot);
    auto r = getLocalBounds().reduced(kPad, kPad).removeFromBottom(kButtons);
    dismiss_.setBounds(r.removeFromRight(84));
    r.removeFromRight(6);
    apply_.setBounds(r.removeFromRight(84));
  }

private:
  static juce::FontOptions titleFont() { return juce::FontOptions(theme::kFontBody, juce::Font::bold); }
  juce::Rectangle<int> titleArea() const {
    auto t = getLocalBounds().reduced(kPad, kPad).withHeight(kLine);
    return t.withWidth(t.getWidth() - 2 * theme::InfoDot::kSlot - 4);
  }

  ProposalsPanel& owner_;
  int index_;
  Item item_;
  theme::InfoDot calls_, notes_{true};
  juce::TextButton apply_, dismiss_;
};

ProposalsPanel::ProposalsPanel() {
  viewport_.setViewedComponent(&content_, false);
  viewport_.setScrollBarsShown(true, false);
  addAndMakeVisible(viewport_);
  setSize(kWidth, 120);
}

ProposalsPanel::~ProposalsPanel() { rows_.clear(); }

void ProposalsPanel::setItems(std::vector<Item> items, const juce::String& emptyText) {
  rows_.clear();
  empty_ = emptyText;
  for (size_t i = 0; i < items.size(); ++i) {
    rows_.push_back(std::make_unique<Row>(*this, (int)i, items[i]));
    content_.addAndMakeVisible(*rows_.back());
  }
  setSize(kWidth, preferredHeight());
  resized();
  repaint();
}

int ProposalsPanel::preferredHeight() const {
  int h = 8;
  for (const auto& r : rows_) h += r->preferredHeight() + 6;
  return rows_.empty() ? 90 : std::min(h, kMaxHeight);
}

void ProposalsPanel::resized() {
  viewport_.setBounds(getLocalBounds());
  int y = 4;
  const int w = getWidth() - (viewport_.isVerticalScrollBarShown() ? viewport_.getScrollBarThickness() : 0);
  for (auto& r : rows_) {
    r->setBounds(4, y, w - 8, r->preferredHeight());
    y += r->preferredHeight() + 6;
  }
  content_.setSize(w, y);
}

void ProposalsPanel::paint(juce::Graphics& g) {
  g.fillAll(theme::kPanel);
  if (rows_.empty()) {
    g.setColour(theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawFittedText(empty_, getLocalBounds().reduced(14), juce::Justification::centred, 4);
  }
}
