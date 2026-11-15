#include "ChoiceDialog.h"
#include "Language.h"
#include "Theme.h"

namespace core::app {

namespace {
constexpr int kBody = 20;  // the room around the question: more than a card's padding, it is the only thing to read
juce::Font messageFont() { return juce::FontOptions(theme::kFontBody); }
}  // namespace

void ChoiceDialog::show(juce::Component& parent, const juce::String& title, const juce::String& message, std::vector<Choice> choices,
                        int cancelCode, std::function<void(int)> done) {
  auto* d = new ChoiceDialog(title, message, std::move(choices), cancelCode, std::move(done));
  parent.addAndMakeVisible(d);
  d->setBounds(parent.getLocalBounds());
  d->toFront(true);
  d->grabKeyboardFocus();
}

ChoiceDialog::ChoiceDialog(const juce::String& title, const juce::String& message, std::vector<Choice> choices, int cancelCode,
                           std::function<void(int)> done)
    : title_(lang::tr(title)), message_(lang::tr(message)), choices_(std::move(choices)), cancel_(cancelCode), done_(std::move(done)) {
  setWantsKeyboardFocus(true);
  for (const auto& c : choices_) {
    auto* b = buttons_.add(new juce::TextButton(c.label));
    b->setClickingTogglesState(false);
    b->setToggleState(c.preferred, juce::dontSendNotification);  // the light button: what Enter does
    const int code = c.code;
    b->onClick = [this, code] { finish(code); };
    addAndMakeVisible(b);
  }
  close_.setButtonText(juce::String::fromUTF8("\xe2\x9c\x95"));
  close_.setTooltip(lang::tr("Cancel"));
  close_.onClick = [this] { finish(cancel_); };
  addAndMakeVisible(close_);
}

int ChoiceDialog::cardHeight() const {
  juce::AttributedString text;
  text.append(message_, messageFont(), theme::kText);
  juce::TextLayout layout;
  layout.createLayout(text, (float)(kWidth - 2 * kBody));
  return theme::kSpaceHeader + kBody + juce::roundToInt(layout.getHeight()) + kBody + theme::kSpaceButton + 2 * theme::kSpacePad + 1;
}

juce::Rectangle<int> ChoiceDialog::card() const {
  return juce::Rectangle<int>(std::min(kWidth, getWidth() - 32), cardHeight()).withCentre(getLocalBounds().getCentre().translated(0, -40));
}

void ChoiceDialog::paint(juce::Graphics& g) {
  g.fillAll(juce::Colours::black.withAlpha(0.45f));  // the window behind, dimmed: it waits for the answer
  const auto r = card();
  g.setColour(theme::kCard);
  g.fillRect(r);
  // The header band: a square mark and the title in capitals, on the left (as every panel title).
  auto head = r.withHeight(theme::kSpaceHeader);
  g.setColour(theme::kPanel.interpolatedWith(theme::kBlueHeading, 0.35f));
  g.fillRect(head);
  g.setColour(theme::kOutline);
  g.fillRect(head.removeFromBottom(1));
  g.setColour(theme::kAccent);
  g.fillRect(juce::Rectangle<float>(6.0f, 6.0f).withPosition((float)r.getX() + theme::kSpacePad, (float)r.getY() + theme::kSpaceHeader / 2 - 3.0f));
  g.setColour(theme::kTitle);
  g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold).withKerningFactor(0.06f));
  g.drawText(title_.toUpperCase(), r.withHeight(theme::kSpaceHeader).withTrimmedLeft(theme::kSpacePad + 14).withTrimmedRight(40),
             juce::Justification::centredLeft, true);
  // The question.
  juce::AttributedString text;
  text.append(message_, messageFont(), theme::kText);
  juce::TextLayout layout;
  layout.createLayout(text, (float)(r.getWidth() - 2 * kBody));
  layout.draw(g, juce::Rectangle<float>((float)r.getX() + kBody, (float)r.getY() + theme::kSpaceHeader + kBody, (float)r.getWidth() - 2 * kBody,
                                        layout.getHeight()));
  // The footer: a line, then the answers.
  const int footer = theme::kSpaceButton + 2 * theme::kSpacePad;
  g.setColour(theme::kOutline);
  g.fillRect(r.getX(), r.getBottom() - footer - 1, r.getWidth(), 1);
  g.drawRect(r, 1);
}

void ChoiceDialog::resized() {
  const auto r = card();
  close_.setBounds(r.withHeight(theme::kSpaceHeader).removeFromRight(theme::kSpaceHeader).reduced(5));
  // The answers on the right of the footer, in the order given, the preferred one last (rightmost).
  auto row = r.withTrimmedTop(r.getHeight() - theme::kSpaceButton - 2 * theme::kSpacePad).reduced(theme::kSpacePad);
  const juce::Font font(juce::FontOptions{theme::kFontBody});
  for (int i = buttons_.size(); --i >= 0;) {
    auto* b = buttons_[i];
    const int w = std::max(96, juce::GlyphArrangement::getStringWidthInt(font, lang::tr(b->getButtonText())) + 2 * theme::kSpacePad + 8);
    b->setBounds(row.removeFromRight(w));
    row.removeFromRight(theme::kSpaceGap);
  }
}

bool ChoiceDialog::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey) { finish(cancel_); return true; }
  if (key == juce::KeyPress::returnKey) {
    for (const auto& c : choices_) if (c.preferred) { finish(c.code); return true; }
  }
  return true;  // the window behind waits for the answer
}

void ChoiceDialog::finish(int code) {
  auto done = std::move(done_);
  juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ChoiceDialog>(this), done, code] {
    if (auto* d = safe.getComponent()) {
      if (auto* p = d->getParentComponent()) p->removeChildComponent(d);
      delete d;
    }
    if (done) done(code);
  });
}

}  // namespace core::app
