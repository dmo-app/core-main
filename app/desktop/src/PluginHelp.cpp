#include "PluginHelp.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>
#include "Theme.h"
#include "Language.h"
#include "fx_racks.hpp"  // doc::styleLabel

namespace core::app {

using namespace theme;

namespace {

juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }

// The role a section of samples is named by: the catalogue's name, and the two track roles it does not have.
juce::String roleTitle(const std::string& role) {
  if (role == "accent") return "Claps & snares";
  if (role == "openhat") return "Open hats";
  if (role == "hat") return "Hats";
  static const std::vector<std::string> plural = {"kick", "snare", "clap", "tom", "crash", "impact", "riser", "pad", "synth", "fill"};
  const bool many = std::find(plural.begin(), plural.end(), role) != plural.end();
  return u8(doc::sampleRoleLabel(role)) + (many ? "s" : "");
}

// Words that wrap to the width they are given: measured and painted with the same layout.
class Text final : public juce::Component {
public:
  Text(const juce::String& text, float size, juce::Colour colour, bool bold = false) : colour_(colour), bold_(bold), size_(size), text_(lang::tr(text)) {
    setInterceptsMouseClicks(false, false);
  }
  int heightFor(int width) {
    juce::TextLayout layout;
    layout.createLayout(attributed(), (float)std::max(10, width));
    return (int)std::ceil(layout.getHeight());
  }
  void paint(juce::Graphics& g) override {
    juce::TextLayout layout;
    layout.createLayout(attributed(), (float)getWidth());
    layout.draw(g, getLocalBounds().toFloat());
  }

private:
  juce::AttributedString attributed() const {
    juce::AttributedString s;
    s.setJustification(juce::Justification::topLeft);
    s.setLineSpacing(size_ * 0.3f);
    s.append(text_, juce::Font(juce::FontOptions(size_, bold_ ? juce::Font::bold : juce::Font::plain)), colour_);
    return s;
  }
  juce::Colour colour_;
  bool bold_;
  float size_;
  juce::String text_;
};

// The window's page: a column of sections, laid out for the viewport's width.
class Page final : public juce::Component {
public:
  Page(doc::PluginHelp help, std::function<void(const std::string&)> tryPreset, std::string style)
      : help_(std::move(help)), tryPreset_(std::move(tryPreset)), style_(std::move(style)) {
    if (style_.empty() && helpHost().songStyle) style_ = helpHost().songStyle();
    if (style_.empty() && !help_.styles.empty()) style_ = help_.styles.front().id;
    build();
  }

  // Lays everything out for this width and sets the page's height (the viewport scrolls it).
  void layoutFor(int width) {
    const int pad = 18, w = width - pad * 2;
    int y = pad;
    rules_.clear();
    for (auto& item : items_) {
      y += item.gapAbove;
      if (item.rule) rules_.push_back(y - 10);
      if (item.text != nullptr) {
        const int x = pad + item.indent;
        const int h = item.text->heightFor(w - item.indent - item.rightRoom);
        item.text->setBounds(x, y, w - item.indent - item.rightRoom, h);
        if (item.number != nullptr) item.number->setBounds(pad, y, item.indent, h);
        // Buttons of the row sit at its right, on its first line.
        int bx = pad + w;
        for (auto* b : item.buttons) {
          const int bw = b->getProperties().contains(kIconProperty) ? 24 : std::max(52, b->getBestWidthForHeight(22) + 4);
          bx -= bw;
          b->setBounds(bx, y - 3, bw, 22);
          bx -= 6;
        }
        y += h;
      } else if (!item.chips.empty()) {  // a row of style chips, wrapped
        int x = pad;
        for (auto* c : item.chips) {
          const int cw = c->getBestWidthForHeight(24) + 12;
          if (x > pad && x + cw > pad + w) { x = pad; y += 30; }
          c->setBounds(x, y, cw, 24);
          x += cw + 6;
        }
        y += 24;
      }
    }
    setSize(width, y + pad);
  }

  void paint(juce::Graphics& g) override {
    g.fillAll(kPanel);
    g.setColour(kOutline.withAlpha(0.6f));
    for (int y : rules_) g.fillRect(18, y, getWidth() - 36, 1);
  }

private:
  struct Item {
    std::unique_ptr<Text> text, number;
    std::vector<juce::TextButton*> buttons;  // owned by the page (owned_)
    std::vector<juce::TextButton*> chips;
    int gapAbove = 0, indent = 0, rightRoom = 0;
    bool rule = false;  // a thin line above (a new section)
  };

  Item& add(const juce::String& words, float size, juce::Colour colour, bool bold, int gap) {
    items_.emplace_back();
    Item& it = items_.back();
    it.text = std::make_unique<Text>(words, size, colour, bold);
    it.gapAbove = gap;
    addAndMakeVisible(*it.text);
    return it;
  }
  void section(const juce::String& title) {
    Item& it = add(title.toUpperCase(), theme::kFontSmall, kTitle, true, 22);
    it.rule = true;
  }
  juce::TextButton* button(const juce::String& text, const juce::String& tip) {
    auto b = std::make_unique<juce::TextButton>(text);
    b->setTooltip(tip);
    addAndMakeVisible(*b);
    owned_.push_back(std::move(b));
    return owned_.back().get();
  }

  void build() {
    add(u8(help_.title), theme::kFontHeading, kText, true, 0);
    add(u8(help_.summary), theme::kFontStrong, kMuted, false, 6);

    if (!help_.steps.empty()) {
      section("How to use it");
      for (size_t i = 0; i < help_.steps.size(); ++i) {
        Item& it = add(u8(help_.steps[i]), theme::kFontBody, kText, false, 8);
        it.indent = 22;
        it.number = std::make_unique<Text>(juce::String((int)i + 1) + ".", theme::kFontBody, kAccent, true);
        addAndMakeVisible(*it.number);
      }
    }

    if (!help_.examples.empty()) {
      section("Examples");
      for (const auto& e : help_.examples) {
        Item& title = add(u8(e.title), theme::kFontBody, kText, true, 10);
        if (!e.preset.empty() && tryPreset_) {
          auto* b = button("Try it", "Puts this example on the plugin now (one undo step)");
          const std::string preset = e.preset;
          b->onClick = [this, preset] { tryPreset_(preset); };
          title.buttons.push_back(b);
          title.rightRoom = 70;
        }
        add(u8(e.text), theme::kFontBody, kMuted, false, 3);
      }
    }

    if (!help_.styles.empty()) {
      section("Styles");
      items_.emplace_back();
      Item& row = items_.back();
      row.gapAbove = 10;
      for (const auto& s : help_.styles) {
        auto* chip = button(u8(doc::styleLabel(s.id)), juce::String());
        chip->setClickingTogglesState(false);
        chip->setToggleState(s.id == style_, juce::dontSendNotification);
        const std::string id = s.id;
        chip->onClick = [this, id] { chooseStyle(id); };
        row.chips.push_back(chip);
      }
      add(styleWhyText(), theme::kFontBody, kMuted, false, 8);
      add("A guide, not a rule: every style still works.", theme::kFontSmall, kDim, false, 4);
    }

    if (!help_.sampleRoles.empty()) buildSamples();
  }

  juce::String styleWhyText() const {
    for (const auto& s : help_.styles)
      if (s.id == style_) return s.why.empty() ? u8(doc::styleLabel(s.id)) : u8(doc::styleLabel(s.id)) + ": " + u8(s.why);
    return {};
  }

  void buildSamples() {
    section(style_.empty() ? juce::String("Samples that go with it") : "Samples for " + u8(doc::styleLabel(style_)));
    const doc::SampleIndex* index = helpHost().index ? helpHost().index() : nullptr;
    if (index == nullptr || index->samples.empty()) {
      Item& it = add("Choose your sample folder to see sounds from your own library here.", theme::kFontBody, kMuted, false, 10);
      if (helpHost().onChooseLibrary) {
        auto* b = button("Choose", "Samples > Choose the library folder");
        b->onClick = [] { helpHost().onChooseLibrary(); };
        it.buttons.push_back(b);
        it.rightRoom = 76;
      }
      return;
    }
    const auto found = doc::helpSamples(*index, help_, style_);
    if (found.empty()) {
      add("No samples of these kinds in your library yet.", theme::kFontBody, kMuted, false, 10);
      return;
    }
    const bool canUse = helpHost().onUse && (!helpHost().canUse || helpHost().canUse());
    for (const auto& group : found) {
      add(roleTitle(group.role), theme::kFontBody, kTitle, true, 12);
      for (const auto& s : group.samples) {
        const doc::LibrarySample* sample = s.sample;
        Item& it = add(u8(sample->displayName.empty() ? sample->name : sample->displayName), theme::kFontBody, kText, false, 6);
        it.indent = 8;
        it.rightRoom = canUse ? 100 : 34;
        const std::string sha = sample->sha256, role = group.role;
        if (helpHost().onPreview) {
          auto* play = button({}, "Hear it once (the song does not change)");
          play->getProperties().set(kIconProperty, "play");
          play->onClick = [sha, role] {
            const auto* idx = helpHost().index ? helpHost().index() : nullptr;
            if (const auto* x = idx ? idx->findHash(sha) : nullptr) helpHost().onPreview(*x, role);
          };
          it.buttons.push_back(play);
        }
        if (canUse) {
          auto* use = button("Use", "Put it on the selected track (one undo step)");
          use->onClick = [sha] {
            const auto* idx = helpHost().index ? helpHost().index() : nullptr;
            if (const auto* x = idx ? idx->findHash(sha) : nullptr) helpHost().onUse(*x);
          };
          it.buttons.push_back(use);
        }
        if (!s.why.empty()) add(u8(s.why), theme::kFontSmall, kDim, false, 1).indent = 8;
      }
    }
  }

  void chooseStyle(const std::string& id);

  doc::PluginHelp help_;
  std::function<void(const std::string&)> tryPreset_;
  std::string style_;
  std::deque<Item> items_;  // a deque: an Item& stays valid while more are added
  std::vector<std::unique_ptr<juce::TextButton>> owned_;
  std::vector<int> rules_;
};

// The window's content: the page in a viewport that only scrolls up and down.
class HelpContent final : public juce::Component {
public:
  HelpContent(doc::PluginHelp help, std::function<void(const std::string&)> tryPreset, std::string style)
      : help_(std::move(help)), tryPreset_(std::move(tryPreset)) {
    view_.setScrollBarsShown(true, false);
    addAndMakeVisible(view_);
    rebuild(std::move(style));
    setSize(440, 560);
  }
  // A new style: the samples change, so the page is made again (and keeps its scroll).
  void rebuild(std::string style) {
    const auto at = view_.getViewPosition();
    auto page = std::make_unique<Page>(help_, tryPreset_, std::move(style));
    view_.setViewedComponent(page.get(), false);  // the old page leaves the viewport before it is deleted
    page_ = std::move(page);
    resized();
    view_.setViewPosition(at);
  }
  void paint(juce::Graphics& g) override { g.fillAll(kPanel); }
  void resized() override {
    view_.setBounds(getLocalBounds());
    if (page_) page_->layoutFor(std::max(200, getWidth() - view_.getScrollBarThickness()));
  }

private:
  doc::PluginHelp help_;
  std::function<void(const std::string&)> tryPreset_;
  juce::Viewport view_;
  std::unique_ptr<Page> page_;
};

void Page::chooseStyle(const std::string& id) {
  // Called from a chip inside this page: the page is replaced after the click has returned.
  juce::Component::SafePointer<juce::Component> self(this);
  juce::MessageManager::callAsync([self, id] {
    if (self == nullptr) return;
    if (auto* content = self->findParentComponentOfClass<HelpContent>()) content->rebuild(id);
  });
}

juce::Component::SafePointer<juce::DialogWindow>& helpWindow() {
  static juce::Component::SafePointer<juce::DialogWindow> window;
  return window;
}

}  // namespace

HelpButton::HelpButton() {
  setWantsKeyboardFocus(false);
  setTooltip("How to use it");
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void HelpButton::paint(juce::Graphics& g) {
  const auto ring = juce::Rectangle<float>(kSize, kSize).withCentre(getLocalBounds().toFloat().getCentre());
  g.setColour(isMouseOver() ? kAccent : kDim);
  g.drawEllipse(ring.reduced(0.75f), 1.5f);
}

void HelpButton::mouseUp(const juce::MouseEvent& e) {
  if (onClick && e.mouseWasClicked() && getLocalBounds().contains(e.getPosition())) onClick();
}

HelpHost& helpHost() {
  static HelpHost host;
  return host;
}

namespace {
juce::File helpFolder() { return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("help"); }
}  // namespace

std::vector<std::string> pluginHelpIds() {
  std::vector<std::string> ids;
  for (const auto& f : helpFolder().findChildFiles(juce::File::findFiles, false, "*.json"))
    ids.push_back(f.getFileNameWithoutExtension().toStdString());
  std::sort(ids.begin(), ids.end());
  return ids;
}

doc::PluginHelp pluginHelpFor(const std::string& id) {
  const juce::File folder = helpFolder();
  try {
    return doc::loadPluginHelp(std::filesystem::path(folder.getFullPathName().toWideCharPointer()), id);
  } catch (const std::exception& e) {
    doc::PluginHelp h;
    h.id = id;
    h.title = id;
    h.summary = std::string("No help for this plugin yet (") + e.what() + ").";
    return h;
  }
}

void showPluginHelp(const doc::PluginHelp& help, std::function<void(const std::string&)> tryPreset, const std::string& style) {
  closePluginHelp();  // one at a time
  juce::DialogWindow::LaunchOptions options;
  options.content.setOwned(new HelpContent(help, std::move(tryPreset), style));
  options.dialogTitle = u8(help.title.empty() ? help.id : help.title) + " - how to use it";
  options.dialogBackgroundColour = kPanel;
  options.escapeKeyTriggersCloseButton = true;
  options.useNativeTitleBar = true;
  options.resizable = true;
  options.componentToCentreAround = helpHost().centreAround;
  helpWindow() = options.launchAsync();
}

void closePluginHelp() {
  if (auto* w = helpWindow().getComponent()) delete w;
  helpWindow() = nullptr;
}

}  // namespace core::app
