#include "EffectsPanel.h"
#include "TrackColour.h"
#include "AttachPanel.h"
#include "attach.hpp"
#include "Language.h"
#include "PluginHelp.h"
#include <algorithm>
#include <cmath>
#include "edit.hpp"
#include "fx_racks.hpp"
#include "rcore.hpp"
#include "fcore.hpp"
#include <map>
#include "Theme.h"
#include "core_plugins.hpp"

namespace core::app {

using json = nlohmann::json;

namespace {
juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }
using theme::kPanel, theme::kCard, theme::kText, theme::kMuted;
constexpr int kRowHeight = 19, kCardHeader = 30;
constexpr int kPathWidth = 230, kChainCardMax = 560;  // the Core Chain: the SIGNAL PATH column, the widest card
constexpr int kKnobCardWidth = 312, kKnobCell = 74, kKnobRow = 90, kCurveHeight = 110;  // knob mode (ref-3)
constexpr int kSideRow = 32;
constexpr int kSummaryH = 40;  // the Core Chain: one line on what the effect does, over its knobs  // a compressor's sidechain row (ref-3: Sidechain, On, Source)
constexpr int kRackCardWidth = 4 * kKnobCell + 24;  // FCore's card

juce::String formatValue(const doc::EffectParamInfo& p, double v) {
  if (p.step > 0 && !p.choices.empty()) return u8(p.choices[(size_t)std::clamp((int)std::lround(v), 0, (int)p.choices.size() - 1)]);
  const std::string unit = p.unit;
  if (unit == "Hz") return v >= 1000 ? juce::String(v / 1000, v >= 10000 ? 1 : 2) + " kHz" : juce::String((int)std::lround(v)) + " Hz";
  if (unit == "ms") return juce::String(v, v < 10 ? 1 : 0) + " ms";
  if (unit == "dB") return juce::String(v, 1) + " dB";
  if (unit == ":1") return juce::String(v, 1) + ":1";
  if (unit == "oct") return (v > 0 ? "+" : "") + juce::String(v, 1) + " oct";
  if (unit == "note") {  // MIDI note: C1 = 24
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int n = (int)std::lround(v);
    return juce::String(names[((n % 12) + 12) % 12]) + juce::String(n / 12 - 1);
  }
  return juce::String(v, 2);
}
}  // namespace

// ---------------------------------------------------------------- one effect
namespace {
// Core's own effects with words in help/<id>.json (read once: the files ship with Core.exe).
bool hasHelp(const std::string& effect) {
  static const std::vector<std::string> ids = pluginHelpIds();
  return std::find(ids.begin(), ids.end(), effect) != ids.end();
}
}  // namespace

class EffectsPanel::Card final : public juce::Component {
public:
  Card(EffectsPanel& owner, int index, const json& fx)
      : owner_(owner), index_(index), name_(doc::effectName(fx)),
        fxId_(fx.contains("id") && fx["id"].is_string() ? fx["id"].get<std::string>() : std::string()) {
    const doc::EffectInfo* info = doc::findEffect(name_);
    title_.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    // One version of each effect (older songs open with the current one): only a version this Core does not know
    // is named, as it plays silent.
    const int version = fx.is_object() && fx.contains("version") && fx["version"].is_number_integer() ? fx["version"].get<int>() : 1;
    const int current = info ? info->version : 0;
    juce::String text = info ? u8(info->label) : u8(name_);
    // An attachment's effect says whose it is: its Mix is written by where the attachment is put.
    if (fx.contains("attach") && fx["attach"].is_object() && fx["attach"].contains("name") && fx["attach"]["name"].is_string()) {
      text = u8(fx["attach"]["name"].get<std::string>()) + u8(" · ") + text;
      title_.setTooltip(u8("Attachment: on only where it is put (notes: Edit > Attachments; clips: right click > Attachments). Its Mix is set there; the other knobs are yours."));
    }
    title_.setText(text, juce::dontSendNotification);
    if (!info || version < 1 || version > current) {
      title_.setText(text + u8(" (unsupported)"), juce::dontSendNotification);
      title_.setColour(juce::Label::textColourId, theme::kError);
      title_.setTooltip(u8("This version of the effect is not in this Core: it plays silent. Remove it, or open the song in the Core it came from."));
    }
    on_.setClickingTogglesState(false);
    on_.setToggleState(!doc::effectBypassed(fx), juce::dontSendNotification);
    on_.setTooltip(u8("On / off (bypass)"));
    on_.onClick = [this] {
      const bool bypass = on_.getToggleState();
      edit([&](doc::Document& d) { doc::setEffectBypass(d, owner_.trackId_, index_, bypass); }, bypass ? u8("Effect off") : u8("Effect on"));
    };
    left_.setButtonText(u8("◀"));
    right_.setButtonText(u8("▶"));
    remove_.setButtonText(u8("✕"));
    left_.setTooltip(u8("Move earlier"));
    right_.setTooltip(u8("Move later"));
    remove_.setTooltip(u8("Remove the effect"));
    left_.onClick = [this] { edit([&](doc::Document& d) { doc::moveEffect(d, owner_.trackId_, index_, index_ - 1); }, u8("Move effect")); };
    right_.onClick = [this] { edit([&](doc::Document& d) { doc::moveEffect(d, owner_.trackId_, index_, index_ + 1); }, u8("Move effect")); };
    remove_.onClick = [this] { edit([&](doc::Document& d) { doc::removeEffect(d, owner_.trackId_, index_); }, u8("Remove effect")); };
    // Save as attachment (Diogo, 2026-11-07: "encontro um que gosto, guardo como attachment"): this effect as it is
    // now, named, into your attachments (every song), to put on a note's or a clip's Start, Middle or End.
    keep_.setButtonText(u8("⤓"));
    keep_.setTooltip(u8("Save as attachment: this effect as it sounds now, with a name, to put on notes and clips (Start, Middle, End) in any song"));
    keepable_ = info && !fx.contains("attach");
    keep_.setVisible(keepable_);
    keep_.onClick = [this] {
      juce::Component::SafePointer<EffectsPanel> panel(&owner_);
      const std::string trackId = owner_.trackId_;
      const int index = index_;
      auto* w = new juce::AlertWindow(u8("Save as attachment"), u8("Its name (any name you like). It will be in every song, in the attachments library under Yours.\nTo keep it in a folder, write the folder first: Glitch/My roll."),
                                      juce::MessageBoxIconType::NoIcon, this);
      w->addTextEditor("name", title_.getText(), u8("Name:"));
      w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
      w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
      juce::Component::SafePointer<juce::AlertWindow> win(w);
      w->enterModalState(true, juce::ModalCallbackFunction::create([win, panel, trackId, index](int ok) {
        if (ok != 1 || !win || !panel) return;
        const std::string name = win->getTextEditorContents("name").trim().toStdString();
        try {
          const json& chain = doc::effectChain(panel->host_.currentDocument(), trackId);
          if (index < 0 || index >= (int)chain.size()) return;
          const json saved = doc::attachmentFromEffect(chain[(size_t)index], name);
          doc::saveUserAttachment(userAttachmentsFolderPath(), saved);
          reloadUserAttachments();
          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, u8("Saved"),
                                                 u8("\"") + u8(name) + u8("\" is in your attachments: Edit > Attachments on notes, right click > Attachments on clips."));
        } catch (const std::exception& e) {
          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, u8("Could not save it"), u8(e.what()));
        }
      }), true);
    };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &on_, &left_, &right_, &remove_, &keep_}) {
      c->setWantsKeyboardFocus(false);
      addAndMakeVisible(c);
    }
    if (info && hasHelp(name_)) {
      // The help ring (PluginHelp.h): how the effect works, examples and styles. "Try it" puts the example's preset
      // on this effect (one undo step). The window can outlive this card, so it keeps the panel, not the card.
      help_.setTooltip(u8("How ") + u8(info->label) + u8(" works: examples and styles"));
      help_.onClick = [this] {
        juce::Component::SafePointer<EffectsPanel> panel(&owner_);
        const std::string effect = name_, trackId = owner_.trackId_;
        const int index = index_;
        showPluginHelp(pluginHelpFor(effect), [panel, effect, trackId, index](const std::string& preset) {
          const doc::EffectPreset* p = doc::findEffectPreset(effect, preset);
          if (panel == nullptr || p == nullptr) return;
          panel->host_.applyEdit([&](doc::Document& d) { doc::applyEffectPreset(d, trackId, index, *p); }, u8("Try ") + u8(preset));
        });
      };
      addAndMakeVisible(help_);
      // In the Core Chain, what it does in one sentence over the knobs (the help's summary).
      if (owner_.chain()) {
        summary_.setText(u8(pluginHelpFor(name_).summary), juce::dontSendNotification);
        summary_.setFont(juce::FontOptions(theme::kFontBody));
        summary_.setColour(juce::Label::textColourId, kMuted);
        summary_.setJustificationType(juce::Justification::topLeft);
        summary_.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(summary_);
        summary_.setVisible(summary_.getText().isNotEmpty());
      }
    }
    if (!info) return;
    if (name_ == "corefx" || name_ == "eq") {  // its own window: the curves over the spectrum, the cards, the LFOs
      open_.setButtonText(u8("Open"));
      open_.setTooltip(u8("Open ") + u8(info->label) + u8(" in its own window (several can be open at once)"));
      open_.setEnabled(!fxId_.empty());
      open_.onClick = [this] { if (owner_.onOpenCoreFx) owner_.onOpenCoreFx(owner_.trackId_, fxId_); };
      open_.setWantsKeyboardFocus(false);
      addAndMakeVisible(open_);
      update(fx);
      return;
    }
    // A track's compressor can be keyed by another track (the engine's sidechain): the kick ducks the bass.
    const doc::Document& song = owner_.host_.currentDocument();
    if (name_ == "compressor" && song.findTrack(owner_.trackId_)) {
      sideOn_.setButtonText(u8("Sidechain"));
      sideOn_.setClickingTogglesState(false);
      sideOn_.setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
      sideOn_.setTooltip(u8("Key the compressor by another track (it ducks when that track plays)"));
      int id = 1;
      for (const auto& t : song.tracks)
        if (t.id != owner_.trackId_) { sideSource_.addItem(u8(t.name), id); sideTracks_.push_back(t.id); ++id; }
      sideSource_.setTextWhenNothingSelected(u8("Source"));
      sideSource_.setTooltip(u8("The track that drives the compressor"));
      sideOn_.onClick = [this] {
        std::string source;
        if (!sideOn_.getToggleState() && !sideTracks_.empty()) {
          source = sideTracks_.front();  // the kick when there is one, else the first other track
          for (size_t i = 0; i < sideTracks_.size(); ++i)
            if (sideSource_.getItemText((int)i).containsIgnoreCase("kick")) { source = sideTracks_[i]; break; }
        }
        edit([&](doc::Document& d) { doc::setEffectSidechain(d, owner_.trackId_, index_, source); }, source.empty() ? u8("Sidechain off") : u8("Sidechain on"));
      };
      sideSource_.onChange = [this] {
        const int i = sideSource_.getSelectedItemIndex();
        if (i < 0 || i >= (int)sideTracks_.size()) return;
        const std::string source = sideTracks_[(size_t)i];
        edit([&](doc::Document& d) { doc::setEffectSidechain(d, owner_.trackId_, index_, source); }, u8("Sidechain source"));
      };
      for (auto* c : std::initializer_list<juce::Component*>{&sideOn_, &sideSource_}) { c->setWantsKeyboardFocus(false); addAndMakeVisible(c); }
    }
    for (const auto& p : info->params) {
      auto label = std::make_unique<juce::Label>();
      label->setText(u8(p.label), juce::dontSendNotification);
      label->setFont(juce::FontOptions(theme::kFontBody));
      label->setColour(juce::Label::textColourId, kMuted);
      // A bar: drag to change, double-click to type (theme::BarSlider).
      // A knob with its name over it and its value (with the unit) under it, as in the W-Core and the brief's panels
      // (Diogo 2026-11-09, the effects: "não estou a perceber"); its arc in the track's colour.
      auto slider = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
      slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, kKnobCell - 4, 16);
      slider->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
      label->setJustificationType(juce::Justification::centred);
      label->setFont(juce::FontOptions(theme::kFontBody));
      label->setColour(juce::Label::textColourId, kText);
      slider->setRange(p.min, p.max, p.step > 0 ? p.step : 0.0);
      if (p.logScale && p.min > 0) slider->setSkewFactorFromMidPoint(std::sqrt(p.min * p.max));
      slider->setDoubleClickReturnValue(true, p.def);
      slider->textFromValueFunction = [&p](double v) { return formatValue(p, v); };
      slider->updateText();  // the box shows the value with its unit from the start (not "0.0000000")
      slider->setTooltip(u8(p.label) + (p.hint ? u8(": ") + u8(p.hint) : juce::String()) + u8(" (drag up or down; double-click: default)"));
      slider->setWantsKeyboardFocus(false);
      const std::string param = p.name;
      juce::Slider* s = slider.get();
      slider->onDragStart = [this] { owner_.host_.startGesture(u8("Effect")); dragging_ = true; };
      slider->onValueChange = [this, s, param] {
        const double v = s->getValue();
        if (curve()) repaint(curveArea());
        edit([&](doc::Document& d) { doc::setEffectParam(d, owner_.trackId_, index_, param, v); }, u8("Effect"));
      };
      slider->onDragEnd = [this] { if (dragging_) owner_.host_.finishGesture(); dragging_ = false; };
      auto automate = std::make_unique<juce::TextButton>("A");
      automate->setTooltip(u8("Automate this value along the song"));
      automate->setWantsKeyboardFocus(false);
      automate->setEnabled(!fxId_.empty() && p.step == 0 && p.automatable);
      automate->setClickingTogglesState(false);
      automate->setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
      automate->onClick = [this, param] { if (owner_.onAutomate) owner_.onAutomate(owner_.trackId_, doc::effectParamKey(fxId_, param)); };
      addAndMakeVisible(*label);
      addAndMakeVisible(*slider);
      addAndMakeVisible(*automate);
      automate->setVisible(owner_.onAutomate != nullptr);
      automate_.push_back(std::move(automate));
      params_.push_back(param);
      labels_.push_back(std::move(label));
      sliders_.push_back(std::move(slider));
    }
    update(fx);
  }

  // A card removed in the middle of a drag (undo while dragging) still ends its gesture.
  ~Card() override { if (dragging_) owner_.host_.finishGesture(); }

  // In the Core Chain the editor's bar is the card's header (On, name, ▾ ▢ ✕): one bar, not two (Diogo 2026-11-09).
  int head() const { return owner_.chain() ? 0 : kCardHeader; }
  int summaryHeight() const { return summary_.getParentComponent() != nullptr && summary_.isVisible() ? kSummaryH : 0; }
  int height() const {
    if (open_.isVisible()) return head() + summaryHeight() + kRowHeight + 16;
    const int side = sideOn_.isVisible() ? kSideRow : 0;
    const int rows = ((int)sliders_.size() + columns() - 1) / columns();
    return head() + summaryHeight() + (curve() ? kCurveHeight + 8 : 0) + rows * kKnobRow + 10 + side;
  }
  static int columns() { return (kKnobCardWidth - 12) / kKnobCell; }
  bool curve() const { return name_ == "compressor"; }
  // For the editor's bar and the SIGNAL PATH's menu (each one acts after the click: the card may be rebuilt).
  void toggleOn() { on_.triggerClick(); }
  bool canMoveEarlier() const { return index_ > 0; }
  void moveEarlier() { left_.triggerClick(); }
  void moveLater() { right_.triggerClick(); }
  void removeIt() { remove_.triggerClick(); }
  bool canKeep() const { return keepable_; }
  void keep() { keep_.triggerClick(); }



  void update(const json fx) {  // by value: the host's document can be replaced while it runs
    on_.setToggleState(!doc::effectBypassed(fx), juce::dontSendNotification);
    if (sideOn_.isVisible()) {
      const std::string source = fx.is_object() ? fx.value("sidechain", std::string()) : std::string();
      sideOn_.setToggleState(!source.empty(), juce::dontSendNotification);
      sideSource_.setEnabled(!source.empty());
      const auto it = std::find(sideTracks_.begin(), sideTracks_.end(), source);
      sideSource_.setSelectedId(it == sideTracks_.end() ? 0 : (int)(it - sideTracks_.begin()) + 1, juce::dontSendNotification);
    }
    for (size_t i = 0; i < sliders_.size(); ++i) {
      if (!sliders_[i]->isMouseButtonDown())
        sliders_[i]->setValue(doc::effectParam(fx, params_[i], sliders_[i]->getDoubleClickReturnValue()), juce::dontSendNotification);
      // An automated value follows its curve: the slider is greyed and its A lit.
      const bool automated = !fxId_.empty() && doc::findAutomation(owner_.host_.currentDocument(), owner_.trackId_, doc::effectParamKey(fxId_, params_[i]));
      sliders_[i]->setEnabled(!automated);
      automate_[i]->setToggleState(automated, juce::dontSendNotification);
    }
    repaint();
  }

  void paint(juce::Graphics& g) override {
    // A card with a header strip (name, on, move, remove), as on the reference's effect cards.
    const auto r = getLocalBounds().toFloat();
    const float alpha = on_.getToggleState() ? 1.0f : 0.55f;
    g.setColour(kCard.withAlpha(alpha));
    g.fillRoundedRectangle(r, theme::kCorner);
    theme::paintCard(g, r, (float)head(), theme::identityOf(*this, {}), alpha);
    if (curve()) paintTransfer(g, curveArea());
  }

  // The compressor's static curve: output level against input level (dB), threshold, ratio and knee.
  juce::Rectangle<int> curveArea() const {
    const int top = head() + summaryHeight() + 6 + (sideOn_.isVisible() ? kSideRow : 0);
    return getLocalBounds().reduced(14, 0).withY(top).withHeight(kCurveHeight).withTrimmedLeft(22);
  }
  void paintTransfer(juce::Graphics& g, juce::Rectangle<int> a) const {
    auto value = [&](const char* p, double def) {
      for (size_t i = 0; i < params_.size(); ++i) if (params_[i] == p) return sliders_[i]->getValue();
      return def;
    };
    const double threshold = value("threshold", -18), ratio = std::max(1.0, value("ratio", 4)), knee = value("knee", 6), makeup = value("makeup", 0);
    g.setColour(theme::kField);
    g.fillRoundedRectangle(a.toFloat(), theme::kCorner);
    auto px = [&](double db) { return a.getX() + (float)(a.getWidth() * (db + 60) / 60); };
    auto py = [&](double db) { return a.getBottom() - (float)(a.getHeight() * juce::jlimit(0.0, 1.0, (db + 60) / 60)); };
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (int db = -60; db <= 0; db += 12) {
      g.setColour(theme::kOutline);
      g.fillRect((float)a.getX(), py(db), (float)a.getWidth(), 1.0f);
      g.fillRect(px(db), (float)a.getY(), 1.0f, (float)a.getHeight());
      g.setColour(theme::kDim);
      g.drawText(juce::String(db), a.getX() - 24, (int)py(db) - 6, 20, 12, juce::Justification::centredRight, false);
    }
    juce::Path p;
    for (int i = 0; i <= 120; ++i) {
      const double in = -60 + i * 0.5;
      const double over = in - threshold;
      double out = in;
      if (2 * over > knee) out = threshold + over / ratio;
      else if (knee > 0 && 2 * std::abs(over) <= knee) out = in + (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
      out += makeup;
      if (i == 0) p.startNewSubPath(px(in), py(out)); else p.lineTo(px(in), py(out));
    }
    g.setColour(theme::kAccent);
    g.strokePath(p, juce::PathStrokeType(1.8f));
    g.setColour(kText);
    g.fillEllipse(px(threshold) - 3.5f, py(threshold + makeup) - 3.5f, 7, 7);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(6, 4);
    if (owner_.chain()) {  // the bar over it has the On, the name and the window buttons; move/remove: the SIGNAL PATH
      for (auto* c : std::initializer_list<juce::Component*>{&title_, &on_, &left_, &right_, &remove_, &keep_}) c->setVisible(false);
      if (summary_.isVisible()) {
        auto row = r.removeFromTop(kSummaryH - 4);
        if (help_.isVisible()) help_.setBounds(row.removeFromRight(HelpButton::kSlot + 4).withSizeKeepingCentre(HelpButton::kSlot, HelpButton::kSlot));
        summary_.setBounds(row);
      } else if (help_.isVisible()) {
        help_.setBounds(r.withHeight(0).removeFromRight(HelpButton::kSlot + 4).withSizeKeepingCentre(HelpButton::kSlot, HelpButton::kSlot).withY(r.getY()));
      }
      r.removeFromTop(4);
      layoutBody(r);
      return;
    }
    auto top = r.removeFromTop(kCardHeader - 6);
    remove_.setBounds(top.removeFromRight(24).reduced(1));
    if (keep_.isVisible()) keep_.setBounds(top.removeFromRight(22).reduced(1));
    right_.setBounds(top.removeFromRight(24).reduced(1));
    left_.setBounds(top.removeFromRight(24).reduced(1));
    on_.setBounds(top.removeFromRight(24).reduced(1));
    if (help_.isVisible()) {  // the ring right after the title's text
      const int textW = juce::GlyphArrangement::getStringWidthInt(title_.getFont(), title_.getText()) + 12;
      const int w = std::min(textW, top.getWidth() - HelpButton::kSlot - 4);
      title_.setBounds(top.removeFromLeft(w));
      help_.setBounds(top.removeFromLeft(HelpButton::kSlot + 4).withSizeKeepingCentre(HelpButton::kSlot, HelpButton::kSlot));
    } else {
      title_.setBounds(top);
    }
    r.removeFromTop(2);
    layoutBody(r);
  }
  void layoutBody(juce::Rectangle<int> r) {
    if (open_.isVisible()) open_.setBounds(r.removeFromTop(kRowHeight + 4).withWidth(90));
    if (sideOn_.isVisible()) {  // under the header, so a short panel still shows it
      auto side = r.removeFromTop(kSideRow).reduced(4, 4);
      sideOn_.setBounds(side.removeFromLeft(96));
      side.removeFromLeft(8);
      sideSource_.setBounds(side);
    }
    // A grid of knobs: the name over each, its value under, A at the corner.
    if (curve()) r.removeFromTop(kCurveHeight + 8);
    const int cols = std::max(1, std::min(columns(), r.getWidth() / kKnobCell)), x0 = r.getX() + (r.getWidth() - cols * kKnobCell) / 2;
    for (size_t i = 0; i < sliders_.size(); ++i) {
      const auto cell = juce::Rectangle<int>(x0 + (int)(i % cols) * kKnobCell, r.getY() + (int)(i / cols) * kKnobRow, kKnobCell, kKnobRow);
      labels_[i]->setBounds(cell.withHeight(16));
      sliders_[i]->setBounds(cell.withTrimmedTop(16).withTrimmedBottom(6).reduced(4, 0));  // a gap before the next row's names
      automate_[i]->setBounds(cell.getRight() - 16, cell.getY() + 16, 14, 14);
    }
  }

  const std::string& name() const { return name_; }
  // The Core Chain's SIGNAL PATH row: its key (the effect id, else its place), its name and whether it is on.
  std::string chainKey() const { return fxId_.empty() ? "#" + std::to_string(index_) : fxId_; }
  juce::String chainName() const { const auto type = filterTypeText(); return type.isEmpty() ? title_.getText() : title_.getText() + u8(" · ") + type; }
  bool isOn() const { return on_.getToggleState(); }

private:
  void edit(const std::function<void(doc::Document&)>& change, const juce::String& label) { owner_.host_.applyEdit(change, label); }

  EffectsPanel& owner_;
  int index_;
  std::string name_, fxId_;
  juce::Label title_;
  // A Filter's type, for its row in the SIGNAL PATH ("Filter · Ladder").
  juce::String filterTypeText() const {
    if (name_ != "filter") return {};
    for (size_t i = 0; i < params_.size(); ++i)
      if (params_[i] == "mode") return sliders_[i]->getTextFromValue(sliders_[i]->getValue());
    return {};
  }
  theme::OnCircle on_;
  juce::TextButton left_, right_, remove_, open_, sideOn_, keep_;
  juce::Label summary_;
  bool keepable_ = false;
  HelpButton help_;
  juce::ComboBox sideSource_;
  std::vector<std::string> sideTracks_;  // the source list's tracks, in its order
  std::vector<std::string> params_;
  std::vector<std::unique_ptr<juce::Label>> labels_;
  std::vector<std::unique_ptr<juce::Slider>> sliders_;
  std::vector<std::unique_ptr<juce::TextButton>> automate_;
  bool dragging_ = false;
};


// ---------------------------------------------------------------- FCore: effects by intent
// One card for the FCore on the track, in layers, so only what can be done in this step is on show:
//   1. "What is this sound for?": one button per intent (the style's first).
//   2. The intent's own mini plugin: "‹ INTRO" goes back; its parts as switches; the four knobs; Sample ▾ or
//      Regions ▾.
// Every explanation (what an intent or a part does, the sentence with the real values, a warning) sits in a
// small grey circle that turns blue and shows it on hover, so the card stays clean. No technical names on show: those stay inside.
class EffectsPanel::FCoreCard final : public juce::Component {
public:
  FCoreCard(EffectsPanel& owner, const doc::FCoreInstance& f)
      : owner_(owner), note_(f.mode == "note"), picking_(owner.expanded_.count(kPick) > 0) {
    title_.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    title_.setText(u8("F-CORE · FX"), juce::dontSendNotification);
    mode_.setText(note_ ? u8("sampler · moves with each note") : u8("effect · moves over its regions"), juce::dontSendNotification);
    mode_.setFont(juce::FontOptions(theme::kFontSmall));
    mode_.setColour(juce::Label::textColourId, kMuted);
    question_.setText(u8("What is this sound for?"), juce::dontSendNotification);
    question_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    question_.setColour(juce::Label::textColourId, kText);
    back_.setTooltip(u8("Back: choose what the sound is for"));
    back_.onClick = [this] { setPicking(true); };
    for (size_t i = 0; i < doc::fcoreStyles().size(); ++i) style_.addItem(u8(doc::fcoreStyles()[i].label), (int)i + 1);
    style_.setTooltip(u8("The style of the music: it shapes the details and puts what suits it first. Nothing is blocked"));
    style_.onChange = [this] {
      const int i = style_.getSelectedItemIndex();
      if (filling_ || i < 0 || i >= (int)doc::fcoreStyles().size()) return;
      const std::string style = doc::fcoreStyles()[(size_t)i].id;
      edit([&](doc::Document& d) { doc::setFCoreStyle(d, owner_.trackId_, style); }, u8("F-Core style"));
    };
    warning_.setWarn(true);
    on_.setClickingTogglesState(false);
    on_.setTooltip(u8("On / off: hear the sound with and without F-Core"));
    on_.onClick = [this] {
      const bool bypass = on_.getToggleState();
      edit([&](doc::Document& d) { doc::setFCoreBypass(d, owner_.trackId_, bypass); }, bypass ? u8("F-Core off") : u8("F-Core on"));
    };
    inside_.setButtonText(u8("Show inside"));
    inside_.setClickingTogglesState(false);
    inside_.setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
    inside_.setTooltip(u8("For the curious: the effects F-Core uses, as their own cards (F-Core writes them again when you change it)"));
    inside_.onClick = [this] {
      if (owner_.expanded_.count(kInside)) owner_.expanded_.erase(kInside); else owner_.expanded_.insert(kInside);
      owner_.shape_ = "?";
      owner_.triggerAsyncUpdate();
    };
    remove_.setButtonText(u8("✕"));
    remove_.setTooltip(u8("Remove F-Core (the sampler and its sample stay)"));
    remove_.onClick = [this] {
      owner_.expanded_.erase(kInside);
      owner_.expanded_.erase(kPick);
      edit([&](doc::Document& d) { doc::removeFCore(d, owner_.trackId_); }, u8("Remove F-Core"));
    };
    extra_.setButtonText(note_ ? u8("Sample ▾") : u8("Regions ▾"));
    extra_.setTooltip(note_ ? u8("The sample it plays: the style's samples first, the others after with a warning")
                            : u8("Where in the song it plays: over the loop region, or remove regions"));
    extra_.onClick = [this] { if (note_) showSampleMenu(); else showRegionMenu(); };
    print_.setButtonText(u8("Print to sample"));
    print_.setTooltip(u8("Record the track over the loop region as a new sample in your library (set the loop over the note first)"));
    print_.onClick = [this] { if (owner_.onPrintFCore) owner_.onPrintFCore(owner_.trackId_); };
    regions_.setFont(juce::FontOptions(theme::kFontSmall));
    regions_.setColour(juce::Label::textColourId, kMuted);
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &mode_, &question_, &back_, &style_, &sentence_, &warning_, &on_, &inside_,
                                                         &remove_, &extra_, &print_, &regions_}) {
      c->setWantsKeyboardFocus(false);
      addChildComponent(c);
    }
    // Layer 1: one button per intent, in the style's order.
    for (const auto& id : doc::fcoreIntentsFor(f.style)) {
      const doc::FCoreIntent* info = doc::findFCoreIntent(id);
      const std::string warn = doc::fcoreIntentWarning(f.style, id);
      auto button = std::make_unique<juce::TextButton>(u8(info->label) + (warn.empty() ? juce::String() : u8("  !")));
      button->setClickingTogglesState(false);
      button->setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
      button->setToggleState(id == f.intent, juce::dontSendNotification);
      button->setTooltip(u8(info->what) + (warn.empty() ? juce::String() : u8(". Careful: ") + u8(warn)));
      button->setWantsKeyboardFocus(false);
      button->onClick = [this, id] { pickIntent(id); };
      auto line = std::make_unique<theme::InfoDot>(!warn.empty());
      line->setTooltip(u8(info->what) + (warn.empty() ? juce::String() : u8(". Careful: ") + u8(warn) + "."));
      addChildComponent(*button);
      addChildComponent(*line);
      intentButtons_.push_back(std::move(button));
      intentLines_.push_back(std::move(line));
    }
    // Layer 2: its steps (Diogo: it starts simple, one step; "+ Add step" adds the next; a preset switches on the
    // steps it needs). Only the steps that play are on show, each with its ✕. Fixed while this card lives: the
    // panel makes a new card when the steps, the intent or the style change.
    for (const auto& part : doc::fcoreIngredients(f.intent, f.style, f.knobs, f.mode)) {
      const std::string id = part.id;
      if (std::find(f.off.begin(), f.off.end(), id) != f.off.end()) { offParts_.push_back(part); continue; }
      auto button = std::make_unique<juce::TextButton>(u8(part.label));
      button->setClickingTogglesState(false);
      button->setToggleState(true, juce::dontSendNotification);
      button->setColour(juce::TextButton::buttonOnColourId, theme::kPrimary.withAlpha(0.55f));
      button->setInterceptsMouseClicks(false, false);  // a name, not a switch: ✕ takes the step away
      auto line = std::make_unique<theme::InfoDot>();
      line->setTooltip(u8(part.what));
      auto x = std::make_unique<juce::TextButton>(u8("✕"));
      x->setWantsKeyboardFocus(false);
      x->onClick = [this, id] { edit([&](doc::Document& d) { doc::setFCoreIngredient(d, owner_.trackId_, id, false); }, u8("F-Core: remove step")); };
      addChildComponent(*button);
      addChildComponent(*line);
      addChildComponent(*x);
      partIds_.push_back(id);
      partButtons_.push_back(std::move(button));
      partLines_.push_back(std::move(line));
      partRemoves_.push_back(std::move(x));
    }
    addStep_.setButtonText(u8("+ Add step"));
    addStep_.setEnabled(!offParts_.empty());
    addStep_.onClick = [this] { showAddStepMenu(); };
    presets_.setButtonText(u8("Presets ▾"));
    presets_.setEnabled(!doc::fcorePresetsOf(f.intent).empty());
    presets_.onClick = [this, intent = f.intent] { showPresetMenu(intent); };
    for (auto* c : std::initializer_list<juce::Component*>{&addStep_, &presets_}) { c->setWantsKeyboardFocus(false); addChildComponent(c); }
    struct Spec { const char* name; const char* hint; };
    const Spec specs[] = {{"Amount", "How much of it: subtle .. extreme"},
                          {"Length", note_ ? "How long it lasts from each note, in bars" : "Regions set their own length (Regions ▾)"},
                          {"Tone", "Dark .. bright"},
                          {"Space", "Dry .. huge room"}};
    for (int i = 0; i < 4; ++i) {
      auto label = std::make_unique<juce::Label>();
      label->setText(u8(specs[i].name), juce::dontSendNotification);
      label->setFont(juce::FontOptions(theme::kFontSmall));
      label->setJustificationType(juce::Justification::centred);
      label->setColour(juce::Label::textColourId, kMuted);
      auto knob = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
      knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, kKnobCell - 4, 16);
      knob->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
      knob->setColour(juce::Slider::rotarySliderFillColourId, theme::kAccent);
      theme::useBar(*knob);  // a blue bar, not a knob (Diogo 2026-11-07)
      if (i == 1) {  // Length, in bars (1/4 bar .. 64 bars)
        knob->setRange(0.25, 64.0, 0.25);
        knob->setSkewFactorFromMidPoint(8.0);
        knob->textFromValueFunction = [](double v) { return juce::String(v, v < 1 ? 2 : 0) + (v == 1 ? u8(" bar") : u8(" bars")); };
        knob->valueFromTextFunction = [](const juce::String& t) { return juce::jlimit(0.25, 64.0, t.getDoubleValue()); };
        knob->setEnabled(note_);
      } else {
        knob->setRange(0.0, 1.0, 0.0);
        knob->setDoubleClickReturnValue(true, i == 0 ? 0.6 : 0.5);
        knob->textFromValueFunction = [](double v) { return juce::String((int)std::lround(v * 100)) + " %"; };
        knob->valueFromTextFunction = [](const juce::String& t) { return juce::jlimit(0.0, 1.0, t.getDoubleValue() / 100.0); };
      }
      knob->updateText();
      auto dot = std::make_unique<theme::InfoDot>();
      dot->setTooltip(u8(specs[i].hint));
      addChildComponent(*dot);
      knobDots_.push_back(std::move(dot));
      knob->setWantsKeyboardFocus(false);
      const juce::String name = u8("F-Core ") + u8(specs[i].name);
      knob->onDragStart = [this, name] { owner_.host_.startGesture(name); dragging_ = true; };
      knob->onValueChange = [this, name] { if (!filling_) setKnobs(name); };
      knob->onDragEnd = [this] { if (dragging_) owner_.host_.finishGesture(); dragging_ = false; };
      addChildComponent(*label);
      addChildComponent(*knob);
      labels_.push_back(std::move(label));
      knobs_.push_back(std::move(knob));
    }
    showLayer();
    update(f, "");
  }
  ~FCoreCard() override { if (dragging_) owner_.host_.finishGesture(); }

  static constexpr const char* kInside = "fcore";      // in expanded_: its effects shown as their own cards
  static constexpr const char* kPick = "fcore:pick";   // in expanded_: the card shows layer 1 (choose the intent)
  static constexpr int kRow = 28, kPartColumns = 3;

  int height() const {
    if (picking_) return kCardHeader + 30 + (int)((intentButtons_.size() + 1) / 2) * kRow + 10;
    const int partRows = (int)(partButtons_.size() + kPartColumns - 1) / kPartColumns;
    return kCardHeader + 18 + 28 + partRows * kRow + kRow + 6 + kKnobRow + 30 + 10;
  }

  void update(const doc::FCoreInstance& f, const std::string& sentence) {
    filling_ = true;
    for (size_t i = 0; i < doc::fcoreStyles().size(); ++i)
      if (doc::fcoreStyles()[i].id == f.style) style_.setSelectedId((int)i + 1, juce::dontSendNotification);
    const doc::FCoreIntent* intent = doc::findFCoreIntent(f.intent);
    back_.setButtonText(u8("‹  ") + u8(intent->label).toUpperCase());
    sentence_.setTooltip(u8(sentence.empty() ? doc::planFCore(f.intent, f.style, f.knobs, f.mode, f.off).sentence : sentence));
    const std::string warn = doc::fcoreIntentWarning(f.style, f.intent);
    warning_.setTooltip(warn.empty() ? juce::String() : u8("Careful: ") + u8(warn) + ".");
    warning_.setVisible(!picking_ && !warn.empty());
    on_.setToggleState(!f.bypass, juce::dontSendNotification);
    inside_.setToggleState(owner_.expanded_.count(kInside) > 0, juce::dontSendNotification);

    const double values[] = {f.knobs.amount, f.knobs.lengthBeats / 4.0, f.knobs.tone, f.knobs.space};
    for (size_t i = 0; i < knobs_.size(); ++i)
      if (!knobs_[i]->isMouseButtonDown()) knobs_[i]->setValue(values[i], juce::dontSendNotification);
    knobs_[1]->setEnabled(note_ && intent->moves);
    if (note_) {
      const doc::Track* t = owner_.host_.currentDocument().findTrack(owner_.trackId_);
      regions_.setText(t && !t->instrument.samples.empty() ? u8(t->instrument.samples[0].name) + u8(" · play it on C4 (other keys transpose)")
                                                           : u8("No sample yet: Sample ▾"), juce::dontSendNotification);
    } else {
      const double bar = 4.0 * doc::kPpq;
      juce::String info;
      for (const auto& x : f.regions)
        info << (info.isEmpty() ? "" : ", ") << juce::String((int)(x.startTick / bar) + 1) << u8("–") << juce::String((int)std::ceil(x.endTick / bar));
      regions_.setText(f.regions.empty() ? u8("No region yet: set the loop over the part, then Regions ▾") : u8("Plays on bars ") + info, juce::dontSendNotification);
    }
    filling_ = false;
    repaint();
  }

  void paint(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat();
    const float alpha = on_.getToggleState() || picking_ ? 1.0f : 0.55f;
    theme::paintCard(g, r, (float)kCardHeader, theme::identityOf(*this, {}), alpha);
    g.setColour(theme::kAccent.withAlpha(0.8f * alpha));
    g.fillRoundedRectangle(r.withWidth(3.0f).reduced(0, 6), 1.5f);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(8, 4);
    auto top = r.removeFromTop(kCardHeader - 6);
    remove_.setBounds(top.removeFromRight(24).reduced(1));
    if (picking_) {
      title_.setBounds(top.removeFromLeft(60));
      mode_.setBounds(top);
      auto row = r.removeFromTop(30).reduced(0, 3);
      style_.setBounds(row.removeFromRight(130));
      question_.setBounds(row);
      const int w = r.getWidth() / 2;
      for (size_t i = 0; i < intentButtons_.size(); ++i) {
        auto cell = juce::Rectangle<int>(r.getX() + (int)(i % 2) * w, r.getY() + (int)(i / 2) * kRow, w, kRow).reduced(3, 2);
        intentLines_[i]->setBounds(cell.removeFromRight(theme::InfoDot::kSlot + 2).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
        intentButtons_[i]->setBounds(cell.withTrimmedRight(2));
      }
      return;
    }
    inside_.setBounds(top.removeFromRight(84).reduced(1));
    on_.setBounds(top.removeFromRight(24).reduced(1));
    back_.setBounds(top.removeFromLeft(std::min(top.getWidth() - 40, 140)).reduced(0, 1));
    sentence_.setBounds(top.removeFromLeft(theme::InfoDot::kSlot + 4).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));  // what it does, with the real values
    warning_.setBounds(top.removeFromLeft(theme::InfoDot::kSlot + 4).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
    regions_.setBounds(r.removeFromTop(18));
    auto pick = r.removeFromTop(28).reduced(0, 2);
    style_.setBounds(pick.removeFromRight(130));
    mode_.setBounds(pick);
    const int w = r.getWidth() / kPartColumns;
    auto parts = r.removeFromTop((int)((partButtons_.size() + kPartColumns - 1) / kPartColumns) * kRow);
    for (size_t i = 0; i < partButtons_.size(); ++i) {
      auto cell = juce::Rectangle<int>(parts.getX() + (int)(i % kPartColumns) * w, parts.getY() + (int)(i / kPartColumns) * kRow, w, kRow).reduced(2, 2);
      partRemoves_[i]->setBounds(cell.removeFromRight(20));
      partLines_[i]->setBounds(cell.removeFromRight(theme::InfoDot::kSlot + 2).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
      partButtons_[i]->setBounds(cell.withTrimmedRight(2));
    }
    auto stepRow = r.removeFromTop(kRow).reduced(2, 2);
    addStep_.setBounds(stepRow.removeFromLeft(100));
    stepRow.removeFromLeft(6);
    presets_.setBounds(stepRow.removeFromLeft(100));
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(30).reduced(0, 3);
    extra_.setBounds(buttons.removeFromLeft(90));
    buttons.removeFromLeft(6);
    if (print_.isVisible()) print_.setBounds(buttons.removeFromLeft(120));
    const int x0 = r.getX() + (r.getWidth() - 4 * kKnobCell) / 2;
    for (size_t i = 0; i < knobs_.size(); ++i) {
      const auto cell = juce::Rectangle<int>(x0 + (int)i * kKnobCell, r.getY(), kKnobCell, kKnobRow);
      labels_[i]->setBounds(cell.withHeight(16));
      // The dot right after the name (the name is centred: about 6 px a letter at this size).
      const int nameWidth = 6 * labels_[i]->getText().length();
      knobDots_[i]->setBounds(cell.getCentreX() + nameWidth / 2 + 2, cell.getY() + 3, theme::InfoDot::kSlot, theme::InfoDot::kSlot);
      knobs_[i]->setBounds(cell.withTrimmedTop(16).withTrimmedBottom(6).reduced(4, 0).withSizeKeepingCentre(cell.getWidth() - 8, theme::kBarH));
    }
  }

private:
  // Only the layer on show is visible.
  void showLayer() {
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &question_}) c->setVisible(picking_);
    for (auto& b : intentButtons_) b->setVisible(picking_);
    for (auto& l : intentLines_) l->setVisible(picking_);
    for (auto* c : std::initializer_list<juce::Component*>{&back_, &sentence_, &on_, &inside_, &extra_, &regions_}) c->setVisible(!picking_);
    print_.setVisible(!picking_ && owner_.onPrintFCore != nullptr);
    for (auto& b : partButtons_) b->setVisible(!picking_);
    for (auto& l : partLines_) l->setVisible(!picking_);
    for (auto& x : partRemoves_) x->setVisible(!picking_);
    addStep_.setVisible(!picking_);
    presets_.setVisible(!picking_);
    for (auto& l : labels_) l->setVisible(!picking_);
    for (auto& k : knobs_) k->setVisible(!picking_);
    for (auto& d : knobDots_) d->setVisible(!picking_);
    mode_.setVisible(true);
    style_.setVisible(true);
    remove_.setVisible(true);
  }

  // The layer changes the card's height: the panel lays the cards out again.
  void setPicking(bool picking) {
    if (picking) owner_.expanded_.insert(kPick); else owner_.expanded_.erase(kPick);
    owner_.shape_ = "?";
    owner_.triggerAsyncUpdate();
  }

  // The next steps, in their order, each with what it does.
  void showAddStepMenu() {
    juce::PopupMenu menu;
    for (size_t i = 0; i < offParts_.size(); ++i)
      menu.addItem((int)i + 1, u8(offParts_[i].label) + u8("  —  ") + u8(offParts_[i].what));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addStep_), [this](int chosen) {
      if (chosen <= 0 || chosen > (int)offParts_.size()) return;
      const std::string id = offParts_[(size_t)chosen - 1].id;
      edit([&](doc::Document& d) { doc::setFCoreIngredient(d, owner_.trackId_, id, true); }, u8("F-Core: add step"));
    });
  }

  // A preset switches on the steps it needs (and the others off).
  void showPresetMenu(const std::string& intent) {
    const auto& presets = doc::fcorePresetsOf(intent);
    juce::PopupMenu menu;
    for (size_t i = 0; i < presets.size(); ++i) menu.addItem((int)i + 1, u8(presets[i].label) + u8("  —  ") + u8(presets[i].what));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presets_), [this, intent](int chosen) {
      const auto& list = doc::fcorePresetsOf(intent);
      if (chosen <= 0 || chosen > (int)list.size()) return;
      const std::string id = list[(size_t)chosen - 1].id;
      edit([&](doc::Document& d) { doc::setFCorePreset(d, owner_.trackId_, id); }, u8("F-Core: ") + u8(list[(size_t)chosen - 1].label));
    });
  }

  void pickIntent(const std::string& intent) {
    const doc::Track* t = owner_.host_.currentDocument().findTrack(owner_.trackId_);
    const auto f = t ? doc::fcoreOf(*t) : std::nullopt;
    owner_.expanded_.erase(kPick);
    if (f && f->intent == intent) { setPicking(false); return; }
    edit([&](doc::Document& d) { doc::setFCoreIntent(d, owner_.trackId_, intent); }, u8("F-Core: ") + u8(doc::findFCoreIntent(intent)->label));
  }

  void edit(const std::function<void(doc::Document&)>& change, const juce::String& label) {
    try {
      doc::Document probe = owner_.host_.currentDocument();  // refused here (a full chain...), before any undo step
      change(probe);
    } catch (const std::exception& e) {
      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, u8("FX Core (F-Core)"), u8(e.what()));
      return;
    }
    owner_.host_.applyEdit(change, label);
  }

  void setKnobs(const juce::String& name) {
    doc::FCoreKnobs k;
    k.amount = knobs_[0]->getValue();
    k.lengthBeats = knobs_[1]->getValue() * 4.0;
    k.tone = knobs_[2]->getValue();
    k.space = knobs_[3]->getValue();
    owner_.host_.applyEdit([&](doc::Document& d) { doc::setFCoreKnobs(d, owner_.trackId_, k); }, name);
  }

  void showRegionMenu() {
    const doc::Document& d = owner_.host_.currentDocument();
    const int64_t start = d.loop.startTick, end = std::min(d.loop.endTick, d.lengthTicks);
    const double bar = 4.0 * doc::kPpq;
    const juce::String bars = u8("bars ") + juce::String((int)(start / bar) + 1) + u8("–") + juce::String((int)std::ceil(end / bar));
    juce::PopupMenu menu;
    menu.addItem(1, u8("Run over the loop region (") + bars + ")", end - start >= doc::kPpq / 4);
    menu.addSeparator();
    menu.addItem(2, u8("Remove the regions in the loop region"));
    menu.addItem(3, u8("Remove every region"));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&extra_), [this, start, end](int chosen) {
      if (chosen <= 0) return;
      const std::string track = owner_.trackId_;
      if (chosen == 1)
        edit([=](doc::Document& x) { doc::removeFCoreRegions(x, track, start, end); doc::addFCoreRegion(x, track, start, end); }, u8("F-Core region"));
      else
        edit([=](doc::Document& x) { doc::removeFCoreRegions(x, track, chosen == 2 ? start : 0, chosen == 2 ? end : 0); }, u8("Remove F-Core regions"));
    });
  }

  // The library's samples for the style: its own first (by role), the others after, each with its warning.
  void showSampleMenu() {
    const doc::SampleIndex* index = owner_.sampleLibrary ? owner_.sampleLibrary() : nullptr;
    const doc::Track* t = owner_.host_.currentDocument().findTrack(owner_.trackId_);
    const auto f = t ? doc::fcoreOf(*t) : std::nullopt;
    if (!index || index->samples.empty() || !f) {
      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, u8("FX Core (F-Core)"), u8("No sample library yet: scan it in the Library (or drag a sample onto the track)."));
      return;
    }
    const std::vector<doc::FCoreSampleChoice> order = doc::fcoreSamples(index->samples, f->style, owner_.host_.currentDocument().bpm);
    std::map<std::string, juce::PopupMenu> mine, others;
    std::map<std::string, int> counts;
    constexpr int kPerRole = 80;  // a menu stays readable; the Library finds the rest
    for (const auto& c : order) {
      const doc::LibrarySample& s = index->samples[c.index];
      const std::string role = s.info.role;
      int& n = counts[(c.forStyle ? "+" : "-") + role];
      if (++n > kPerRole) continue;
      juce::String text = u8(s.displayName.empty() ? s.name : s.displayName);
      if (!c.warning.empty()) text << u8("  — ") << u8(c.warning);
      (c.forStyle ? mine : others)[role].addItem((int)c.index + 1, text);
    }
    juce::PopupMenu menu;
    menu.addSectionHeader(u8("For ") + u8(doc::findFCoreStyle(f->style)->label));
    if (mine.empty()) menu.addItem(juce::PopupMenu::Item(u8("None marked with the style or at its tempo")).setEnabled(false));
    for (auto& [role, sub] : mine) menu.addSubMenu(u8(role), sub);
    menu.addSeparator();
    juce::PopupMenu rest;
    for (auto& [role, sub] : others) rest.addSubMenu(u8(role), sub);
    menu.addSubMenu(u8("Others (some may give trouble: the reason is next to them)"), rest, !others.empty());
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&extra_), [this, index](int chosen) {
      if (chosen <= 0 || chosen - 1 >= (int)index->samples.size()) return;
      const doc::SampleRef zone = doc::sampleRef(index->samples[(size_t)chosen - 1]);
      const std::string track = owner_.trackId_;
      edit([=](doc::Document& d) { doc::trackById(d, track).instrument.samples = {zone}; }, u8("F-Core sample"));
    });
  }

  EffectsPanel& owner_;
  bool note_ = true, picking_ = false, filling_ = false, dragging_ = false;
  juce::Label title_, mode_, question_, regions_;
  theme::InfoDot sentence_, warning_{true};
  juce::ComboBox style_;
  theme::OnCircle on_;
  juce::TextButton back_, inside_, remove_, extra_, print_;
  std::vector<std::unique_ptr<juce::TextButton>> intentButtons_, partButtons_, partRemoves_;
  std::vector<doc::FCoreIngredient> offParts_;  // the steps "+ Add step" offers, in order
  juce::TextButton addStep_, presets_;
  std::vector<std::unique_ptr<theme::InfoDot>> intentLines_, partLines_, knobDots_;
  std::vector<std::string> partIds_;
  std::vector<std::unique_ptr<juce::Label>> labels_;
  std::vector<std::unique_ptr<juce::Slider>> knobs_;
};

// ---------------------------------------------------------------- the Core Chain's SIGNAL PATH
// One row per step of the chain, in the order it plays: the instrument, F-Core, then each effect. A row has a dot
// (filled in the track's colour when on, an empty ring when off) and the name; the chosen row has the 2 px line in the
// track's colour, as a selected card. A click chooses the effect whose card shows on the right; the instrument row
// opens the instrument.
class EffectsPanel::ChainList final : public juce::Component {
public:
  explicit ChainList(EffectsPanel& owner) : owner_(owner) { setWantsKeyboardFocus(false); }
  struct Row {
    std::string key;
    juce::String name, note;  // note: a word on the right in grey ("Instrument")
    bool on = true;
  };
  static constexpr int kRow = 30, kGap = 4;
  void setRows(std::vector<Row> rows) { rows_ = std::move(rows); setSize(getWidth(), height()); repaint(); }
  int height() const { return (int)rows_.size() * (kRow + kGap); }
  bool has(const std::string& key) const {
    return std::any_of(rows_.begin(), rows_.end(), [&](const Row& r) { return r.key == key; });
  }
  void setOn(const std::string& key, bool on) {
    for (auto& r : rows_) if (r.key == key && r.on != on) { r.on = on; repaint(); }
  }

  void paint(juce::Graphics& g) override {
    const juce::Colour ident = theme::identityOf(*this, theme::kBlueSoft);
    const std::string chosen = owner_.selectedKey();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const Row& row = rows_[i];
      const auto r = rowArea((int)i).toFloat();
      const bool sel = row.key == chosen, over = (int)i == hover_;
      g.setColour(over ? theme::kBlueHeading : sel ? theme::kBlueDeep : theme::kCard);
      g.fillRoundedRectangle(r, theme::kCorner);
      g.setColour(sel ? ident : theme::kOutline);
      const float w = sel ? 2.0f : 1.0f;
      g.drawRoundedRectangle(r.reduced(w / 2), theme::kCorner, w);
      const auto dot = juce::Rectangle<float>(8.0f, 8.0f).withCentre({r.getX() + 14.0f, r.getCentreY()});
      g.setColour(row.on ? ident : theme::kDim);
      if (row.on) g.fillEllipse(dot); else g.drawEllipse(dot.reduced(0.5f), 1.2f);
      auto text = r.withTrimmedLeft(26.0f).withTrimmedRight(8.0f);
      if (row.note.isNotEmpty()) {
        g.setColour(theme::kMuted);
        g.setFont(juce::FontOptions(theme::kFontSmall));
        g.drawText(lang::tr(row.note), text.removeFromRight(70.0f), juce::Justification::centredRight, true);
      }
      g.setColour(row.on ? kText : kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody, sel ? juce::Font::bold : juce::Font::plain));
      g.drawText(row.name, text, juce::Justification::centredLeft, true);
    }
  }
  void mouseMove(const juce::MouseEvent& e) override { setHover(rowAt(e.getPosition())); }
  void mouseExit(const juce::MouseEvent&) override { setHover(-1); }
  void mouseUp(const juce::MouseEvent& e) override {
    const int i = rowAt(e.getPosition());
    if (i < 0 || !e.mouseWasClicked()) return;
    const std::string key = rows_[(size_t)i].key;
    if (key == kInstrument) { if (owner_.onOpenInstrument) owner_.onOpenInstrument(owner_.trackId_); return; }
    if (e.mods.isPopupMenu()) { owner_.showEffectMenu(key, *this); return; }  // move, save, remove (the editor's bar has none)
    owner_.select(key);
  }
  static constexpr const char* kInstrument = "instrument";

private:
  juce::Rectangle<int> rowArea(int i) const { return {0, i * (kRow + kGap), getWidth(), kRow}; }
  int rowAt(juce::Point<int> p) const {
    const int i = p.y / (kRow + kGap);
    return i >= 0 && i < (int)rows_.size() && rowArea(i).contains(p) ? i : -1;
  }
  void setHover(int i) { if (i != hover_) { hover_ = i; repaint(); } }

  EffectsPanel& owner_;
  std::vector<Row> rows_;
  int hover_ = -1;
};

// ---------------------------------------------------------------- + Add effect: the picker
// Diogo 2026-11-09: "ainda menu antigo de efeitos". + Add effect opens this list in the chain itself, not a pop-up
// menu: Core's effects as tiles in groups (Core plugins, shared Reverb / Delay, filters, the other effects), each
// with one line on what it does. A plugin with presets opens its presets here (as it comes first, then this song's
// style, then the others); ‹ All effects goes back. Only the newest version of an effect is offered (no old ones).
class EffectsPanel::Picker final : public juce::Component {
public:
  struct Tile {
    juce::String name, tip;
    std::function<void()> act;
    bool enabled = true;
    juce::Rectangle<int> area;
  };
  struct Group { juce::String title; std::vector<Tile> tiles; };

  explicit Picker(EffectsPanel& owner) : owner_(owner) {
    back_.setButtonText(u8("‹ All effects"));
    back_.onClick = [this] { show({}); };
    close_.setButtonText(u8("Close"));
    close_.onClick = [this] { owner_.setPickerOpen(false); };
    for (auto* b : {&back_, &close_}) { b->setWantsKeyboardFocus(false); addAndMakeVisible(b); }
    show({});
  }
  // page "": every effect; "presets:<i>": the presets of builtinEffects()[i]; "rcore" / "dcore": the shared places.
  void show(const std::string& page) {
    page_ = page;
    groups_ = page.empty() ? rootPage() : page == "rcore" || page == "dcore" ? placesPage(page == "rcore") : presetsPage(page);
    back_.setVisible(!page.empty());
    hover_ = {-1, -1};
    layoutFor(getWidth());
    if (auto* v = findParentComponentOfClass<juce::Viewport>()) v->setViewPosition(0, 0);
  }
  int layoutFor(int width) {  // places the tiles; returns the height they need
    const int cols = std::max(1, (width + kGap) / (kTileW + kGap));
    const int w = std::max(kTileW - 40, (width - (cols - 1) * kGap) / cols);
    int y = kTop;
    for (auto& g : groups_) {
      y += kTitleH;
      for (size_t i = 0; i < g.tiles.size(); ++i) {
        const int c = (int)i % cols;
        if (i > 0 && c == 0) y += kTileH + kGap;
        g.tiles[i].area = {c * (w + kGap), y, w, kTileH};
      }
      y += kTileH + 16;
    }
    height_ = y;
    return y;
  }
  int height() const { return height_; }
  void resized() override {
    auto top = getLocalBounds().removeFromTop(kTop - 8);
    close_.setBounds(top.removeFromRight(80));
    top.removeFromRight(8);
    back_.setBounds(top.removeFromLeft(130));
  }
  void paint(juce::Graphics& g) override {
    const juce::Colour ident = theme::identityOf(*this, theme::kBlueSoft);
    g.setColour(theme::kBlueLight);
    g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    if (!back_.isVisible()) g.drawText(lang::tr(u8("Add effect")), 0, 0, 300, kTop - 8, juce::Justification::centredLeft);
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
      const auto& grp = groups_[gi];
      if (grp.tiles.empty()) continue;
      // Rule 11: each group a tone of the track's scale, light at the top; its title mark and its tiles' edges.
      int shown = 0;
      for (const auto& gg : groups_) shown += gg.tiles.empty() ? 0 : 1;
      int nth = 0;
      for (size_t k = 0; k < gi; ++k) nth += groups_[k].tiles.empty() ? 0 : 1;
      const juce::Colour tone = theme::toneScale(ident, shown, nth);
      theme::paintSectionTitle(g, juce::Rectangle<float>(0.0f, (float)grp.tiles.front().area.getY() - kTitleH, 400.0f, (float)kTitleH - 6), grp.title, tone);
      for (size_t ti = 0; ti < grp.tiles.size(); ++ti) {
        const Tile& t = grp.tiles[ti];
        const bool over = t.enabled && hover_ == std::make_pair((int)gi, (int)ti);
        // Dark, and light under the mouse (the buttons' rule): the words are always the opposite of the fill.
        const auto r = t.area.toFloat().reduced(0.5f);
        g.setColour(over ? theme::kBlueLight : theme::kControl);
        g.fillRoundedRectangle(r, theme::kCorner);
        g.setColour(over ? theme::kBlueLight : tone);
        g.drawRoundedRectangle(r, theme::kCorner, 1.0f);
        const float a = t.enabled ? 1.0f : 0.4f;
        auto text = t.area.reduced(10, 6);
        g.setColour((over ? theme::kBackground : kText).withMultipliedAlpha(a));
        g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
        g.drawText(t.name, text.removeFromTop(18), juce::Justification::centredLeft, true);
        g.setColour((over ? theme::kBlueDeep : kMuted).withMultipliedAlpha(a));
        g.setFont(juce::FontOptions(theme::kFontSmall));
        g.drawFittedText(t.tip, text, juce::Justification::topLeft, 2, 0.9f);
      }
    }
  }
  void mouseMove(const juce::MouseEvent& e) override { setHover(tileAt(e.getPosition())); }
  void mouseExit(const juce::MouseEvent&) override { setHover({-1, -1}); }
  void mouseUp(const juce::MouseEvent& e) override {
    const auto at = tileAt(e.getPosition());
    if (at.first < 0 || !e.mouseWasClicked()) return;
    const Tile& t = groups_[(size_t)at.first].tiles[(size_t)at.second];
    if (t.enabled && t.act) { const auto act = t.act; act(); }  // may close the picker (and delete this)
  }

private:
  static constexpr int kTileW = 230, kTileH = 54, kGap = 8, kTitleH = 26, kTop = 40;
  std::pair<int, int> tileAt(juce::Point<int> p) const {
    for (size_t g = 0; g < groups_.size(); ++g)
      for (size_t t = 0; t < groups_[g].tiles.size(); ++t)
        if (groups_[g].tiles[t].area.contains(p)) return {(int)g, (int)t};
    return {-1, -1};
  }
  void setHover(std::pair<int, int> h) { if (h != hover_) { hover_ = h; repaint(); } }

  static bool isDevice(const std::string& n) {
    return n == "corefx" || n == "growl" || n == "metal" || n == "punch" || n == "zap" || n == "glitch" || n == "pulse" || n == "body";
  }
  static juce::String aboutOf(const std::string& effect) {  // the plugin's own line when Core has one
    for (const auto& p : doc::corePlugins())
      if (p.id == effect || std::find(p.aliases.begin(), p.aliases.end(), effect) != p.aliases.end()) return u8(p.about);
    return {};
  }
  std::vector<Group> rootPage() {
    const auto& all = doc::builtinEffects();
    const auto& presets = doc::effectPresets();
    const bool full = !owner_.add_.isEnabled();
    std::vector<Group> out;
    Group plugins{u8("Core plugins"), {}}, shared{u8("Shared by the selected tracks"), {}}, filters{u8("Filters"), {}}, rest{u8("Effects"), {}};
    for (size_t i = 0; i < all.size(); ++i) {
      const std::string name = all[i].name;
      if (name == "filter") continue;  // the filters have their own group, one tile per type
      const bool hasPresets = std::any_of(presets.begin(), presets.end(), [&](const doc::EffectPreset& p) { return name == p.effect; });
      Tile t{u8(all[i].label), aboutOf(name), {}, !full};
      if (hasPresets) {
        t.tip = t.tip.isEmpty() ? u8("Presets for each style") : t.tip;
        t.act = [this, i] { show("presets:" + std::to_string(i)); };
      } else {
        t.act = [this, name] { owner_.addEffect(name); owner_.setPickerOpen(false); };
      }
      (isDevice(name) ? plugins : rest).tiles.push_back(std::move(t));
    }
    if (!owner_.trackId_.empty()) {
      shared.tiles.push_back({u8("Reverb · SPACE"), u8("One reverb on a bus, a send from each selected track; it follows the sections"),
                              [this] { show("rcore"); }, true});
      shared.tiles.push_back({u8("Delay · ECHO"), u8("One delay on a bus, a send from each selected track; it follows the sections"),
                              [this] { show("dcore"); }, true});
    }
    const bool roomForFilter = owner_.filterCount() < kMaxFilters && !full;
    for (const auto& f : filterTypes())
      filters.tiles.push_back({u8(f.name), u8(f.what), [this, mode = f.mode] { owner_.addFilter(mode); owner_.setPickerOpen(false); }, roomForFilter});
    if (!owner_.trackId_.empty())
      for (const int bars : {8, 16})
        filters.tiles.push_back({u8("Comb at the phrase ends · ") + juce::String(bars) + u8(" bars"),
                                 u8("The last bar of every phrase rings and falls: marks the end of a phrase"),
                                 [this, bars] {
                                   const std::vector<std::string> tracks = owner_.targets();
                                   owner_.host_.applyEdit([&](doc::Document& d) { for (const auto& t : tracks) doc::addPhraseEndComb(d, t, bars); },
                                                          u8("Comb at the phrase ends"));
                                   owner_.scrollToEnd_ = true;
                                   owner_.setPickerOpen(false);
                                 }, roomForFilter});
    for (auto* g : {&plugins, &shared, &filters, &rest}) if (!g->tiles.empty()) out.push_back(std::move(*g));
    if (full) out.insert(out.begin(), Group{u8("This chain is full (16 effects): remove one first"), {}});
    return out;
  }
  std::vector<Group> presetsPage(const std::string& page) {
    const size_t i = (size_t)std::stoul(page.substr(8));
    const auto& all = doc::builtinEffects();
    if (i >= all.size()) return {};
    const std::string name = all[i].name, style = owner_.currentStyle();
    const auto& presets = doc::effectPresets();
    Group start{u8(all[i].label), {}}, own{u8("This song's style"), {}}, others{u8("Other styles"), {}};
    start.tiles.push_back({u8("As it comes"), u8("Its own starting values"), [this, name] { owner_.addEffect(name); owner_.setPickerOpen(false); }, true});
    for (size_t k = 0; k < presets.size(); ++k) {
      if (name != presets[k].effect) continue;
      (presets[k].style == style ? own : others).tiles.push_back(
          {u8(doc::styleLabel(presets[k].style)) + u8(" · ") + u8(presets[k].name), u8(presets[k].tip), [this, k] {
             const doc::EffectPreset& preset = doc::effectPresets()[k];
             const std::vector<std::string> tracks = owner_.targets();
             const doc::EffectInfo* info = doc::findEffect(preset.effect);
             owner_.host_.applyEdit([&](doc::Document& d) { for (const auto& t : tracks) doc::addEffect(d, t, preset); },
                                    u8("Add ") + (info ? u8(info->label) : u8(preset.effect)) + u8(" · ") + u8(preset.name));
             owner_.scrollToEnd_ = true;
             owner_.setPickerOpen(false);
           }, true});
    }
    std::vector<Group> out{std::move(start)};
    if (!own.tiles.empty()) out.push_back(std::move(own));
    if (!others.tiles.empty()) out.push_back(std::move(others));
    return out;
  }
  std::vector<Group> placesPage(bool reverb) {
    const auto& list = reverb ? doc::rcorePresets() : doc::dcorePresets();
    const std::string style = owner_.currentStyle();
    Group own{reverb ? u8("Reverb · this song's style") : u8("Delay · this song's style"), {}}, others{u8("Other styles"), {}};
    for (size_t k = 0; k < list.size(); ++k)
      (list[k].style == style ? own : others).tiles.push_back(
          {u8(doc::styleLabel(list[k].style)) + u8(" · ") + u8(list[k].name), u8(list[k].tip), [this, reverb, k] {
             const doc::RCorePreset& place = (reverb ? doc::rcorePresets() : doc::dcorePresets())[k];
             const std::vector<std::string> tracks = owner_.targets();
             owner_.host_.applyEdit([&](doc::Document& d) { doc::applyRCore(d, tracks, place); }, u8(place.gesture) + u8(" · ") + u8(place.name));
             owner_.setPickerOpen(false);
           }, true});
    std::vector<Group> out;
    if (!own.tiles.empty()) out.push_back(std::move(own));
    if (!others.tiles.empty()) out.push_back(std::move(others));
    return out;
  }
  struct FilterType { int mode; const char* name; const char* what; };
  static const std::vector<FilterType>& filterTypes() {
    static const std::vector<FilterType> list = {
        {0, "Low-pass", "Keeps the lows: darker, further away"},
        {1, "High-pass", "Takes the lows away: thinner, makes room for kick and bass"},
        {2, "Band-pass", "Only the middle around the cutoff: a telephone, a radio"},
        {6, "Notch", "Everything but a narrow band: a hollow, phasey sound"},
        {3, "Low 6", "A gentle low-pass: softens the top, very natural"},
        {4, "High 6", "A gentle high-pass: thins the bottom a little"},
        {10, "Low 24", "A steep low-pass: closes hard, the classic synth sweep"},
        {11, "High 24", "A steep high-pass: cuts the bottom off cleanly"},
        {5, "Band 24", "A narrow band: thin and focused"},
        {7, "Ladder", "The warm analog low-pass (Moog style): fat, it sings with resonance"},
        {12, "Diode", "The acid filter (TB-303): it squelches, the bass thins out as resonance rises"},
        {13, "MS-20 Low", "Aggressive low-pass: it screams and growls with resonance and Drive"},
        {14, "MS-20 High", "Aggressive high-pass: a biting, screaming top"},
        {9, "Formant", "Vowels: the cutoff moves it A-E-I-O-U, a talking sound"},
        {17, "Morph", "Low, band or high by Shape: automate Shape to morph between them"},
        {8, "Comb", "Tunes the sound to the cutoff: metallic, flanged, plucked"},
        {15, "Comb -", "The hollow comb: odd harmonics, an octave under the cutoff"},
        {16, "Phaser", "Moving notches: the psychedelic sweep (Shape spreads them)"},
        {18, "Dual Notch", "Two notches around the cutoff, Shape sets how far apart"},
        {19, "Dual Band", "Two bands around the cutoff: a vocal, two-peaked sound"}};
    return list;
  }

  EffectsPanel& owner_;
  juce::TextButton back_, close_;
  std::string page_;
  std::vector<Group> groups_;
  std::pair<int, int> hover_{-1, -1};
  int height_ = 0;
};

// ---------------------------------------------------------------- the panel

EffectsPanel::EffectsPanel(PatternEditor::Host& host) : host_(host), ab_(host) {
  title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, kText);
  hint_.setFont(juce::FontOptions(theme::kFontBody));
  hint_.setColour(juce::Label::textColourId, kMuted);
  hint_.setJustificationType(juce::Justification::centredRight);
  hint_.setText(u8("Effects play in this order, before the track volume · Alt+click a value: its default"), juce::dontSendNotification);
  add_.setButtonText(u8("+ Add effect"));
  add_.setTooltip(u8("Core's effects, filters and shared reverb / delay, to put at the end of this chain"));
  add_.onClick = [this] { setPickerOpen(!picker_); };
  automation_.setButtonText(u8("Automation"));
  automation_.setTooltip(u8("The volume (and effect values, with each one's A button) along the song"));
  automation_.onClick = [this] { if (onAutomate) onAutomate(trackId_, "gainDb"); };
  attach_.setButtonText(u8("Attachments"));
  attach_.setTooltip(u8("Start, Middle and End of the chosen clip (or the track's first one): which attachment sounds "
                        "there, beside the effect cards, so you can set both without going back. Click again to close."));
  attach_.onClick = [this] { if (onAttachments) onAttachments(trackId_); };
  close_.setButtonText(u8("✕"));
  close_.setTooltip(u8("Close (Esc): back to the pattern editor"));
  close_.onClick = [this] { if (onClose) onClose(); };
  setCollapsed(false);
  minimise_.onClick = [this] { if (onMinimise) onMinimise(); };
  minimise_.setWantsKeyboardFocus(false);
  addChildComponent(minimise_);
  // Style FX: the style first (the song's own when it came from a template), then the racks made for it.
  for (size_t i = 0; i < doc::rackStyles().size(); ++i) style_.addItem(u8(doc::styleLabel(doc::rackStyles()[i])), (int)i + 1);
  style_.setTooltip(u8("The style the Style FX racks are made for"));
  rackButton_.setButtonText(u8("Style FX ▾"));
  rackButton_.setTooltip(u8("Several of Core's own effects at once, set for the style: effects, build-ups"));
  rackButton_.onClick = [this] { showRackMenu(); };
  fcoreButton_.setButtonText(u8("FX ▾"));
  fcoreButton_.setTooltip(u8("Effects by intent: choose what the sound is for (build-up, intro, impact...) and a style; one card, four knobs"));
  fcoreButton_.onClick = [this] { showFCoreMenu(); };
  duckButton_.setButtonText(u8("Duck ▾"));
  duckButton_.onClick = [this] { showDuckMenu(); };
  duckDot_.setTooltip(u8("Duck: this track's volume dips at every note of another track (the bass at every kick), so they "
                         "don't fight. Applies to all the selected tracks."));
  for (auto* c : std::initializer_list<juce::Component*>{&style_, &rackButton_, &fcoreButton_, &duckButton_, &duckDot_}) {
    c->setWantsKeyboardFocus(false);
    addChildComponent(c);
  }
  for (auto* c : std::initializer_list<juce::Component*>{&title_, &hint_, &add_, &automation_, &attach_, &close_, &ab_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
  view_.setViewedComponent(&strip_, false);
  view_.setScrollBarsShown(true, true);  // a card taller than the panel scrolls (small windows)
  addAndMakeVisible(view_);
  list_ = std::make_unique<ChainList>(*this);
  listView_.setViewedComponent(list_.get(), false);
  listView_.setScrollBarsShown(true, false);  // a long chain scrolls down
  listView_.setWantsKeyboardFocus(false);
  addAndMakeVisible(listView_);
}


// ---------------------------------------------------------------- the Core Chain's editors
class EffectsPanel::EditorBar final : public juce::Component {
public:
  // floating: the bar of an unpinned editor's window: ▾ minimises the window, ▢ docks it back, ✕ closes the view.
  // card: the effect's card under it, whose On the bar shows (the bar is its header); nullptr for F-Core.
  EditorBar(EffectsPanel& o, std::string key, juce::String name, juce::String track, bool floating, Card* card = nullptr)
      : owner_(o), key_(std::move(key)), name_(std::move(name)), track_(std::move(track)), floating_(floating) {
    if (card != nullptr) {
      on_.setToggleState(card->isOn(), juce::dontSendNotification);
      on_.setTooltip(lang::tr(u8("On / off (bypass)")));
      juce::Component::SafePointer<Card> c(card);
      on_.onClick = [c] { if (c) c->toggleOn(); };
      addAndMakeVisible(on_);
    }
    minimise_.setButtonText(u8("\xe2\x96\xbe"));
    minimise_.setTooltip(lang::tr(u8("Minimise to the side (the effect keeps playing)")));
    // Each action runs after the click has returned: it rebuilds the bars, this one with them.
    if (floating) minimise_.onClick = [this] { if (auto* w = getTopLevelComponent()) if (auto* peer = w->getPeer()) peer->setMinimised(true); };
    else minimise_.onClick = [this] { later([](EffectsPanel& p, const std::string& k) { p.minimiseEditor(k, true); }); };
    // The square keeps its drawing in both states (the brief): detach here, dock in its own window.
    unpin_.setButtonText(u8("\xe2\x96\xa2"));
    unpin_.setTooltip(lang::tr(floating ? u8("Dock: back in the Core Chain") : u8("Detach: its own window, which can go outside Core's window")));
    if (floating) unpin_.onClick = [this] { if (owner_.soloDone_) owner_.soloDone_(true); };  // it defers itself
    else unpin_.onClick = [this] { later([](EffectsPanel& p, const std::string& k) { p.unpinEditor(k); }); };
    close_.setButtonText(u8("\xe2\x9c\x95"));
    close_.setTooltip(lang::tr(u8("Close the editor (the effect stays in the chain)")));
    if (floating) close_.onClick = [this] { if (owner_.soloDone_) owner_.soloDone_(false); };
    else close_.onClick = [this] { later([](EffectsPanel& p, const std::string& k) { p.closeEditor(k); }); };
    for (auto* b : {&minimise_, &unpin_, &close_}) { b->setWantsKeyboardFocus(false); addAndMakeVisible(b); }
  }
  void paint(juce::Graphics& g) override {
    // Charcoal with the track's colour as a line on top (the brief: identity, not a body filled with it).
    const juce::Colour ident = theme::identityOf(owner_, theme::kBlueSoft);
    auto r = getLocalBounds();
    g.setColour(kCard);
    g.fillRect(r);
    g.setColour(theme::kOutline);
    g.drawRect(r);
    g.setColour(ident);
    g.fillRect(r.withHeight(2));
    r.removeFromLeft(8);
    if (on_.isVisible()) {
      r.removeFromLeft(24);  // the On circle
    } else {
      g.setColour(ident);
      g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({(float)r.getX() + 3.5f, (float)r.getCentreY()}));
      r.removeFromLeft(14);
    }
    r.removeFromRight(3 * 26 + 6);
    const juce::Font bold(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    g.setFont(bold);
    g.setColour(theme::kText);
    const int w = std::min(r.getWidth(), juce::GlyphArrangement::getStringWidthInt(bold, name_) + 10);
    g.drawText(name_, r.removeFromLeft(w), juce::Justification::centredLeft, true);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.setColour(theme::kMuted);
    g.drawText(track_, r, juce::Justification::centredLeft, true);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(3, 3);
    on_.setBounds(getLocalBounds().withX(6).withWidth(22));
    close_.setBounds(r.removeFromRight(24));
    r.removeFromRight(2);
    unpin_.setBounds(r.removeFromRight(24));
    r.removeFromRight(2);
    minimise_.setBounds(r.removeFromRight(24));
  }
  static constexpr int kHeight = 28;

private:
  void later(void (*action)(EffectsPanel&, const std::string&)) {
    juce::Component::SafePointer<EffectsPanel> safe(&owner_);
    const std::string key = key_;
    juce::MessageManager::callAsync([safe, key, action] { if (safe) action(*safe, key); });
  }
  EffectsPanel& owner_;
  std::string key_;
  juce::String name_, track_;
  bool floating_;
  theme::OnCircle on_;
  juce::TextButton minimise_, unpin_, close_;
};

// ---------------------------------------------------------------- an unpinned editor: its own window
class EffectsPanel::FloatWindow final : public juce::DocumentWindow {
public:
  FloatWindow(EffectsPanel& owner, const std::string& trackId, const std::string& key, juce::Rectangle<int> from)
      : juce::DocumentWindow(u8("Core"), kPanel, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton) {
    panel_ = new EffectsPanel(owner.host_);
    panel_->solo_ = key;
    panel_->onOpenCoreFx = owner.onOpenCoreFx;
    panel_->onAutomate = owner.onAutomate;
    // Pin back, or the effect left the chain: after the click (this window, the panel with it, goes).
    panel_->soloDone_ = [safe = juce::Component::SafePointer<EffectsPanel>(&owner), trackId, key](bool pin) {
      juce::MessageManager::callAsync([safe, trackId, key, pin] { if (safe) safe->dropFloating(trackId, key, pin); });
    };
    close_ = [done = panel_->soloDone_] { done(false); };
    setUsingNativeTitleBar(true);  // a window of the system's: it can go anywhere, on any screen
    setContentOwned(panel_, false);
    setResizable(true, false);
    setResizeLimits(220, 160, 2400, 1600);
    panel_->setTarget(trackId);
    panel_->handleUpdateNowIfNeeded();  // its card now, for the size
    const juce::Component* card = panel_->soloCard();
    const int w = std::max(kKnobCardWidth, card ? card->getWidth() : kKnobCardWidth) + 24;
    const int h = std::clamp((card ? card->getHeight() : 300) + EditorBar::kHeight + 28, 200, 900);
    setContentComponentSize(w, h);
    setTopLeftPosition(from.isEmpty() ? juce::Point<int>(160, 160) : from.getPosition().translated(24, -h / 2));
    // Never off the screen (the brief: windows outside the screen are recovered).
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getBounds()))
      setBounds(getBounds().constrainedWithin(display->userArea));
    retitle();
    setVisible(true);
    toFront(true);
  }
  EffectsPanel& panel() { return *panel_; }
  void retitle() {
    const doc::Track* t = panel_->trackId_.empty() ? nullptr : panel_->host_.currentDocument().findTrack(panel_->trackId_);
    juce::String name = panel_->solo_ == "fcore" ? u8("F-Core") : juce::String();
    if (const auto* c = panel_->soloCard(); c && name.isEmpty())
      for (const auto& cc : panel_->cards_) if (cc.get() == c) name = cc->chainName();
    setName(name + u8(" \xc2\xb7 ") + (t ? u8(t->name) : juce::String("Master")));
  }
  // The window's ✕ closes the view (the effect stays in the chain), the same as the editor's ✕.
  void closeButtonPressed() override { setVisible(false); close_(); }

private:
  EffectsPanel* panel_ = nullptr;
  std::function<void()> close_;
};

void EffectsPanel::setCollapsed(bool collapsed) {
  minimise_.setButtonText(collapsed ? u8("\xe2\x96\xb4") : u8("\xe2\x96\xbe"));
  minimise_.setTooltip(lang::tr(collapsed ? u8("Open the Core Chain again (its editors as they were)")
                                          : u8("Minimise the Core Chain to its header (its editors stay as they are)")));
}

EffectsPanel::~EffectsPanel() {
  floating_.clear();
  cancelPendingUpdate();
  picker_.reset();
  listView_.setViewedComponent(nullptr, false);
  list_.reset();
  cards_.clear();
  fcoreCard_.reset();
}

namespace {
std::string floatingKey(const std::string& trackId, const std::string& key) { return trackId + "\n" + key; }
}  // namespace

// Right-click on an effect of the SIGNAL PATH: what the card's header had (move, save as attachment, remove).
void EffectsPanel::showEffectMenu(const std::string& key, juce::Component& at) {
  Card* card = nullptr;
  for (auto& c : cards_) if (c->chainKey() == key) card = c.get();
  if (!card) return;
  juce::PopupMenu menu;
  menu.addSectionHeader(card->chainName());
  menu.addItem(1, lang::tr(u8("Open its editor")));
  menu.addItem(2, lang::tr(u8("Detach: its own window")));
  menu.addSeparator();
  menu.addItem(3, lang::tr(u8("Move earlier")), card->canMoveEarlier());
  menu.addItem(4, lang::tr(u8("Move later")));
  if (card->canKeep()) menu.addItem(5, lang::tr(u8("Save as attachment...")));
  menu.addSeparator();
  menu.addItem(6, lang::tr(u8("Remove the effect")));
  juce::Component::SafePointer<EffectsPanel> safe(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at).withMousePosition(), [safe, key](int r) {
    if (!safe || r <= 0) return;
    if (r == 1) { safe->select(key); return; }
    if (r == 2) { safe->unpinEditor(key); return; }
    for (auto& c : safe->cards_)
      if (c->chainKey() == key) {
        if (r == 3) c->moveEarlier();
        if (r == 4) c->moveLater();
        if (r == 5) c->keep();
        if (r == 6) c->removeIt();
        return;
      }
  });
}

void EffectsPanel::unpinEditor(const std::string& key) {
  if (!solo_.empty() || key.empty()) return;
  juce::Rectangle<int> from;
  for (const auto& b : bars_) if (b->isShowing()) { from = b->getScreenBounds(); break; }
  auto& open = openList();
  open.erase(std::remove(open.begin(), open.end(), key), open.end());
  minimised_[trackId_].erase(key);
  auto& w = floating_[floatingKey(trackId_, key)];
  if (!w) w = std::make_unique<FloatWindow>(*this, trackId_, key, from);
  w->toFront(true);
  resized();
  list_->repaint();
  repaint();
}

void EffectsPanel::pinEditor(const std::string& trackId, const std::string& key) { dropFloating(trackId, key, true); }

void EffectsPanel::dropFloating(const std::string& trackId, const std::string& key, bool pin) {
  floating_.erase(floatingKey(trackId, key));
  if (pin) {
    auto& open = open_[trackId];
    if (std::find(open.begin(), open.end(), key) == open.end()) open.push_back(key);
    minimised_[trackId].erase(key);
    selected_[trackId] = key;
  }
  if (trackId != trackId_) return;
  resized();
  list_->repaint();
  repaint();
}

int EffectsPanel::unpinnedEditors() const {
  const std::string prefix = trackId_ + "\n";
  return (int)std::count_if(floating_.begin(), floating_.end(), [&](const auto& f) { return f.first.rfind(prefix, 0) == 0; });
}

juce::Component* EffectsPanel::soloCard() const {
  if (solo_ == "fcore") return fcoreCard_.get();
  for (const auto& c : cards_) if (c->chainKey() == solo_) return c.get();
  return nullptr;
}

class EffectsPanel::SideTab final : public juce::Button {
public:
  // Minimised, the triangle, the square and the X stay at hand (the brief): a click on the tab opens it again (its ▸),
  // ▢ detaches it into its own window, ✕ closes the view.
  SideTab(EffectsPanel& o, std::string key, juce::String name) : juce::Button(name), owner_(o), key_(std::move(key)) {
    setTooltip(lang::tr(u8("Click: open it again. Right-click: close it.")));
    setWantsKeyboardFocus(false);
    unpin_.setButtonText(u8("\xe2\x96\xa2"));
    unpin_.setTooltip(lang::tr(u8("Detach: its own window, which can go outside Core's window")));
    close_.setButtonText(u8("\xe2\x9c\x95"));
    close_.setTooltip(lang::tr(u8("Close the editor (the effect stays in the chain)")));
    unpin_.onClick = [this] { later(true); };
    close_.onClick = [this] { later(false, true); };
    for (auto* b : {&unpin_, &close_}) { b->setWantsKeyboardFocus(false); addAndMakeVisible(b); }
  }
  static constexpr int kButtons = 2 * 24 + 6;
  void resized() override {
    auto r = getLocalBounds().reduced(3).removeFromBottom(2 * 24 + 2);
    close_.setBounds(r.removeFromBottom(24));
    r.removeFromBottom(2);
    unpin_.setBounds(r.removeFromBottom(24));
  }
  void later(bool unpin, bool close = false) {
    juce::Component::SafePointer<EffectsPanel> safe(&owner_);
    const std::string key = key_;
    juce::MessageManager::callAsync([safe, key, unpin, close] {  // after the click: the tabs are rebuilt
      if (!safe) return;
      if (unpin) safe->unpinEditor(key); else if (close) safe->closeEditor(key); else safe->minimiseEditor(key, false);
    });
  }
  void clicked(const juce::ModifierKeys& m) override {
    juce::Component::SafePointer<EffectsPanel> safe(&owner_);
    const std::string key = key_;
    const bool close = m.isPopupMenu();
    juce::MessageManager::callAsync([safe, key, close] {  // after the click: the tabs are rebuilt
      if (!safe) return;
      if (close) safe->closeEditor(key); else safe->minimiseEditor(key, false);
    });
  }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const bool light = over || down;
    auto r = getLocalBounds();
    g.setColour(light ? theme::kBlueLight : theme::kControl);
    g.fillRect(r);
    g.setColour(theme::kOutline);
    g.drawRect(r);
    g.setColour(theme::identityOf(owner_, theme::kBlueSoft));
    g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({r.getCentreX() + 0.0f, r.getY() + 10.0f}));
    g.setColour(light ? theme::kBackground : theme::kText);
    theme::drawIcon(g, "left", juce::Rectangle<float>(10.0f, 10.0f).withCentre({r.getCentreX() + 0.0f, r.getY() + 24.0f}));  // open again
    juce::Graphics::ScopedSaveState state(g);
    const auto c = r.withTrimmedTop(32).withTrimmedBottom(kButtons).getCentre().toFloat();
    g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi, c.x, c.y));
    g.setColour(light ? theme::kBackground : theme::kText);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(getName(), juce::Rectangle<float>((float)r.getHeight() - 36 - kButtons, (float)r.getWidth()).withCentre(c), juce::Justification::centred);
  }

private:
  EffectsPanel& owner_;
  std::string key_;
  juce::TextButton unpin_, close_;
};

std::vector<std::string>& EffectsPanel::openList() { return open_[trackId_]; }

void EffectsPanel::tidyEditors() {
  auto& open = openList();
  open.erase(std::remove_if(open.begin(), open.end(), [&](const std::string& k) {
               if (k == "fcore") return fcoreCard_ == nullptr;
               return std::none_of(cards_.begin(), cards_.end(), [&](const auto& c) { return c->chainKey() == k; });
             }), open.end());
  auto& mini = minimised_[trackId_];
  for (auto it = mini.begin(); it != mini.end();) it = std::find(open.begin(), open.end(), *it) == open.end() ? mini.erase(it) : std::next(it);
}

int EffectsPanel::openEditors() const {
  const auto o = open_.find(trackId_);
  const auto m = minimised_.find(trackId_);
  return o == open_.end() ? 0 : (int)o->second.size() - (m == minimised_.end() ? 0 : (int)m->second.size());
}

int EffectsPanel::minimisedEditors() const {
  const auto m = minimised_.find(trackId_);
  return m == minimised_.end() ? 0 : (int)m->second.size();
}

void EffectsPanel::minimiseEditor(const std::string& key, bool minimised) {
  auto& open = openList();
  if (std::find(open.begin(), open.end(), key) == open.end()) return;
  if (minimised) {
    minimised_[trackId_].insert(key);
  } else {
    minimised_[trackId_].erase(key);
    selected_[trackId_] = key;
  }
  resized();
  list_->repaint();
  repaint();
}

void EffectsPanel::closeEditor(const std::string& key) {
  auto& open = openList();
  open.erase(std::remove(open.begin(), open.end(), key), open.end());
  minimised_[trackId_].erase(key);
  if (selected_[trackId_] == key) selected_[trackId_] = open.empty() ? std::string() : open.back();
  resized();
  list_->repaint();
  repaint();
}

void EffectsPanel::setPickerOpen(bool open) {
  if (open == (picker_ != nullptr)) return;
  if (open) {
    picker_ = std::make_unique<Picker>(*this);
    strip_.addAndMakeVisible(*picker_);
  } else {
    // Closed after the click that chose an effect has returned (the picker never deletes itself in its own handler).
    auto* dying = picker_.release();
    dying->setVisible(false);
    juce::MessageManager::callAsync([dying] { delete dying; });
  }
  resized();
  repaint();
}

// The picker takes the card area while it is open: the cards are hidden under it.
bool EffectsPanel::layoutPicker(juce::Rectangle<int> r) {
  if (!picker_) return false;
  if (fcoreCard_) fcoreCard_->setVisible(false);
  for (auto& c : cards_) c->setVisible(false);
  const int w = r.getWidth() - 12;
  const int h = std::max(picker_->layoutFor(w), r.getHeight() - 12);
  picker_->setBounds(0, 0, w, h);
  strip_.setSize(w, h);
  return true;
}

std::string EffectsPanel::selectedKey() const {
  const auto it = selected_.find(trackId_);
  return it == selected_.end() ? std::string() : it->second;
}

void EffectsPanel::select(const std::string& key) {
  selected_[trackId_] = key;
  if (const auto f = floating_.find(floatingKey(trackId_, key)); f != floating_.end()) {  // unpinned: its window comes up
    f->second->setVisible(true);
    f->second->toFront(true);
    list_->repaint();
    return;
  }
  if (!key.empty() && key != ChainList::kInstrument) {  // its editor opens (or comes back from the side)
    auto& open = openList();
    if (std::find(open.begin(), open.end(), key) == open.end()) open.push_back(key);
    minimised_[trackId_].erase(key);
  }
  resized();
  list_->repaint();
  repaint();
}

juce::Component* EffectsPanel::selectedCard() const {
  const std::string key = selectedKey();
  if (key == "fcore" && fcoreCard_) return fcoreCard_.get();
  for (const auto& c : cards_) if (c->chainKey() == key) return c.get();
  return nullptr;
}

void EffectsPanel::setTarget(const std::string& trackId) {
  if (trackId != trackId_) picker_.reset();  // another track: its own chain, not the list
  trackId_ = trackId;
  ab_.setTrack(trackId, true);  // "" = the master's chain
  shape_ = "?";  // rebuild
  documentChanged();
}

std::string EffectsPanel::shapeOf() const {
  std::string shape = trackId_ + "|";
  try {
    for (const auto& fx : doc::effectChain(host_.currentDocument(), trackId_))
      shape += doc::effectName(fx) + (doc::effectBypassed(fx) ? "-" : "+") + (doc::isFCoreEffect(fx) ? "F" : "") + ",";
    if (const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_)) {
      // A new card for another intent or style (its parts change) and for the other layer (its height changes).
      if (const auto f = doc::fcoreOf(*t))
        shape += "|fcore:" + f->mode + ":" + f->intent + ":" + f->style + ":" + std::to_string(f->off.size()) +
                 [&] { std::string o; for (const auto& x : f->off) o += x + ","; return o; }() + (expanded_.count(FCoreCard::kInside) ? "+" : "") +
                 (expanded_.count(FCoreCard::kPick) ? "?" : "");
    }
  } catch (const std::exception&) { shape += "(none)"; }
  return shape;
}

bool EffectsPanel::racksHere() const {
  return racks != nullptr && !trackId_.empty() && host_.currentDocument().findTrack(trackId_) != nullptr;
}

// The style chosen in the dropdown; at first the song's (its template's style), else dark psycore.
std::string EffectsPanel::currentStyle() const {
  const int i = style_.getSelectedItemIndex();
  if (i >= 0 && i < (int)doc::rackStyles().size()) return doc::rackStyles()[(size_t)i];
  const juce::String song = songStyle ? u8(songStyle()).toLowerCase() : juce::String();
  if (song.contains("slambient")) return "slambient";
  if (song.contains("hi-tech") || song.contains("hitech")) return "hitech";
  if (song.contains("psycore")) return "darkpsycore";
  if (song.contains("psy") || song.contains("twilight") || song.contains("full-on") || song.contains("fullon") || song.contains("progressive"))
    return "psytrance";
  return "darkpsycore";
}

void EffectsPanel::showRackMenu() {
  if (!racksHere()) return;
  const std::vector<doc::RackEntry> all = racks();
  const std::string style = currentStyle();
  // Racks are Core's own presets (Diogo, 2026-11-05): a rack puts them on the selected tracks (ids 1 + index in `all`).
  auto item = [&](juce::PopupMenu& menu, size_t i) {
    const doc::RackDef& r = all[i].def;
    juce::String text = u8(r.name);
    if (!r.role.empty() && r.role != "build-up") text << u8("  · ") << u8(r.role);
    if (!all[i].builtin) text << u8("  · yours");
    const bool ready = doc::nativeRackReady(r);
    if (!ready) text << u8("  — not in this Core yet");
    menu.addItem(juce::PopupMenu::Item(text).setID((int)i + 1).setEnabled(ready));
  };
  auto fits = [&](const doc::RackDef& r, const std::string& s) { return std::find(r.styles.begin(), r.styles.end(), s) != r.styles.end(); };
  juce::PopupMenu menu, fxMenu, riseMenu, every;
  for (size_t i = 0; i < all.size(); ++i) {
    const doc::RackDef& r = all[i].def;
    if (!fits(r, style)) continue;
    if (r.role == "build-up") item(riseMenu, i);
    else item(fxMenu, i);
  }
  menu.addSectionHeader(u8(doc::styleLabel(style)) + u8(" (change it in the dropdown)"));
  menu.addSubMenu(u8("Effects"), fxMenu, fxMenu.getNumItems() > 0);
  menu.addSubMenu(u8("Build-up & roll"), riseMenu, riseMenu.getNumItems() > 0);
  menu.addSeparator();
  for (const auto& s : doc::rackStyles()) {
    juce::PopupMenu sub;
    for (size_t i = 0; i < all.size(); ++i) if (fits(all[i].def, s)) item(sub, i);
    every.addSubMenu(u8(doc::styleLabel(s)), sub, sub.getNumItems() > 0);
  }
  menu.addSubMenu(u8("Every style"), every);
  if (onOpenRacksFolder) menu.addItem(9000, u8("Open the folder of your racks"));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&rackButton_), [this, all](int chosen) {
    if (chosen == 9000) { if (onOpenRacksFolder) onOpenRacksFolder(); return; }
    if (chosen >= 1 && chosen - 1 < (int)all.size()) applyNativeRack(all[(size_t)chosen - 1].def);
  });
}

void EffectsPanel::applyNativeRack(const doc::RackDef& def) {
  const std::vector<std::string> tracks = targets();
  std::vector<std::string> later;
  try {
    doc::Document probe = host_.currentDocument();  // refused here (a full chain), before any undo step
    later = doc::applyNativeRack(probe, tracks, def);
  } catch (const std::exception& e) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, u8("Style FX"), u8(e.what()));
    return;
  }
  host_.applyEdit([&](doc::Document& d) { doc::applyNativeRack(d, tracks, def); }, u8(def.name));
  scrollToEnd_ = true;
  if (!later.empty()) {
    juce::String text = u8("Added Core's own effects. Not in this Core yet: ");
    for (size_t i = 0; i < later.size(); ++i) text << (i ? ", " : "") << u8(later[i]);
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, u8(def.name), text);
  }
}

// The FCore style of the song: its template's style when it came from one, else dark psycore.
std::string EffectsPanel::fcoreStyleOfSong() const {
  const juce::String song = songStyle ? u8(songStyle()).toLowerCase() : juce::String();
  if (song.contains("slambient")) return "slambient";
  if (song.contains("hi-tech") || song.contains("hitech")) return "hitech";
  if (song.contains("psycore")) return "darkpsycore";
  if (song.contains("darkpsy") || song.contains("dark psy")) return "darkpsy";
  if (song.contains("twilight")) return "twilight";
  if (song.contains("full-on") || song.contains("fullon")) return "fullon";
  if (song.contains("progressive")) return "progressive";
  return "darkpsycore";
}

// The tracks a plugin goes on: the selected ones (rule for every Core plugin), the shown track first.
std::vector<std::string> EffectsPanel::fcoreTargets() const {
  std::vector<std::string> out{trackId_};
  for (const auto& id : host_.selectedTracks())
    if (id != trackId_ && host_.currentDocument().findTrack(id)) out.push_back(id);
  return out;
}

// Designed ducking on the selected tracks: which track's notes make them dip, and how (a preset), in one Undo.
void EffectsPanel::showDuckMenu() {
  const doc::Document& d = host_.currentDocument();
  const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_);
  if (!t) return;
  const std::vector<std::string> targets = fcoreTargets();
  const auto current = doc::duckingSource(*t);
  std::vector<std::string> sources;
  juce::PopupMenu menu;
  menu.addSectionHeader(targets.size() > 1 ? u8("Dip the ") + juce::String((int)targets.size()) + u8(" selected tracks at every note of")
                                           : u8("Dip at every note of"));
  const auto& presets = doc::duckPresets();
  for (const auto& src : d.tracks) {
    if (!doc::playsPatterns(src) || std::find(targets.begin(), targets.end(), src.id) != targets.end()) continue;
    juce::PopupMenu sub;
    for (size_t p = 0; p < presets.size(); ++p)
      sub.addItem((int)(sources.size() * 10 + p + 1), u8(presets[p].label) + u8("  —  ") + u8(presets[p].what));
    menu.addSubMenu(u8(src.name) + (current && *current == src.id ? u8("  (now)") : juce::String()), sub);
    sources.push_back(src.id);
  }
  if (sources.empty()) menu.addItem(juce::PopupMenu::Item(u8("No other track with notes")).setEnabled(false));
  menu.addSeparator();
  menu.addItem(juce::PopupMenu::Item(u8("Stop ducking")).setID(100000).setEnabled(current.has_value() || targets.size() > 1));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&duckButton_), [this, targets, sources](int chosen) {
    if (chosen <= 0) return;
    std::function<void(doc::Document&)> change;
    juce::String label;
    if (chosen == 100000) {
      change = [targets](doc::Document& x) {
        for (const auto& id : targets)
          if (const doc::Track* tr = x.findTrack(id); tr && doc::duckingSource(*tr)) doc::removeDucking(x, id);
      };
      label = u8("Stop ducking");
    } else {
      const size_t s = (size_t)(chosen - 1) / 10, p = (size_t)(chosen - 1) % 10;
      if (s >= sources.size() || p >= doc::duckPresets().size()) return;
      const std::string source = sources[s], preset = doc::duckPresets()[p].id;
      change = [targets, source, preset](doc::Document& x) { for (const auto& id : targets) doc::setDuckingPreset(x, id, source, preset); };
      label = u8("Duck: ") + u8(doc::duckPresets()[p].label);
    }
    try {
      doc::Document probe = host_.currentDocument();
      change(probe);
    } catch (const std::exception& e) {
      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, u8("Duck"), u8(e.what()));
      return;
    }
    host_.applyEdit(change, label);
  });
}

void EffectsPanel::showFCoreMenu() {
  const doc::Document& d = host_.currentDocument();
  const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_);
  if (!t) return;
  const std::vector<std::string> targets = fcoreTargets();
  const auto f = doc::fcoreOf(*t);
  juce::PopupMenu menu;
  if (targets.size() > 1) {  // the selected tracks at once, fitted to each; then each one can be changed alone
    int samplers = 0, withFCore = 0;
    for (const auto& id : targets) {
      const doc::Track& x = *d.findTrack(id);
      samplers += x.instrument.kind == "builtin" && x.instrument.name == "sampler";
      withFCore += doc::fcoreOf(x).has_value();
    }
    const juce::String n = juce::String((int)targets.size());
    menu.addSectionHeader(n + u8(" selected tracks"));
    menu.addItem(juce::PopupMenu::Item(u8("F-Core on all ") + n + u8(": samplers move with each note, the others over regions"))
                     .setID(1).setEnabled(samplers > 0));
    menu.addItem(2, u8("F-Core effect on all ") + n + u8(": over regions of the song"));
    if (withFCore > 0) { menu.addSeparator(); menu.addItem(3, u8("Remove F-Core from the selected tracks")); }
  } else {
    const bool sampler = t->instrument.kind == "builtin" && t->instrument.name == "sampler";
    menu.addItem(juce::PopupMenu::Item(u8("F-Core sampler: the movement starts at each note") + (sampler ? juce::String() : u8("  — needs a sampler track")))
                     .setID(1).setEnabled(sampler && !(f && f->mode == "note")));
    menu.addItem(juce::PopupMenu::Item(u8("F-Core effect: the movement runs over regions of the song")).setID(2).setEnabled(!(f && f->mode == "region")));
    if (f) { menu.addSeparator(); menu.addItem(3, u8("Remove F-Core")); }
  }
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&fcoreButton_), [this, targets](int chosen) {
    if (chosen == 1) applyFCore("note");
    else if (chosen == 2) applyFCore("region");
    else if (chosen == 3) {
      expanded_.erase(FCoreCard::kInside);
      host_.applyEdit([&](doc::Document& x) {
        for (const auto& id : targets)
          if (const doc::Track* tr = x.findTrack(id); tr && doc::fcoreOf(*tr)) doc::removeFCore(x, id);
      }, u8("Remove F-Core"));
    }
  });
}

bool EffectsPanel::applyFCore(const std::string& mode) {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  if (!t) return false;
  // A track that has FCore keeps its intent, style and knobs; a new one starts with the style's first intent.
  const auto old = doc::fcoreOf(*t);
  const std::string style = old ? old->style : fcoreStyleOfSong();
  const std::string intent = old ? old->intent : doc::fcoreIntentsFor(style).front();
  doc::FCoreKnobs knobs = old ? old->knobs : doc::FCoreKnobs{};
  if (!old) knobs.lengthBeats = doc::findFCoreIntent(intent)->defaultBeats;
  const std::string target = trackId_;
  const std::vector<std::string> targets = fcoreTargets();
  const std::vector<doc::FCoreRegion> regions = old ? old->regions : std::vector<doc::FCoreRegion>{};
  // One step (one Undo) for every selected track; refused here, before any undo step, when one does not take it.
  auto change = [&](doc::Document& d) {
    if (targets.size() > 1) doc::applyFCoreToTracks(d, targets, intent, style, knobs, mode, regions);
    else doc::applyFCore(d, target, intent, style, knobs, mode);
  };
  try {
    doc::Document probe = host_.currentDocument();
    change(probe);
  } catch (const std::exception& e) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, u8("FX Core (F-Core)"), u8(e.what()));
    return false;
  }
  if (!old) expanded_.insert(FCoreCard::kPick);  // a new FCore opens on its first layer: what is this sound for?
  const juce::String label = targets.size() > 1 ? u8("F-Core on ") + juce::String((int)targets.size()) + u8(" tracks")
                                                : mode == "note" ? u8("F-Core sampler") : u8("F-Core effect");
  host_.applyEdit(change, label);
  return true;
}

void EffectsPanel::documentChanged() {
  for (auto& [k, w] : floating_) { w->panel().documentChanged(); w->retitle(); }  // the unpinned editors follow too
  ab_.documentChanged();
  identifyWithTrack(*this, host_.currentDocument(), trackId_);  // knobs and bars in the track's colour
  const doc::Document& d = host_.currentDocument();
  const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_);
  if (!trackId_.empty() && !t) { shape_.clear(); triggerAsyncUpdate(); return; }  // the track went away
  title_.setText(u8("Core Chain · ") + (t ? u8(t->name) : juce::String("Master")), juce::dontSendNotification);
  hint_.setText(t ? u8("Effects play in this order, before the track volume · Alt+click a value: its default")
                  : u8("The master: the whole mix, after the master volume · Alt+click a value: its default"), juce::dontSendNotification);
  // A copy: a card's update asks the host for its document again, and a host that composes one (the Master
  // page's) replaces the one this would point into.
  const json chain = doc::effectChain(d, trackId_);
  const std::string shape = shapeOf();
  if (shape != shape_) {
    triggerAsyncUpdate();
  } else {
    for (size_t i = 0; i < cards_.size() && i < chain.size(); ++i) cards_[i]->update(chain[i]);
    for (const auto& c : cards_) list_->setOn(c->chainKey(), c->isOn());
  }
  add_.setEnabled((int)chain.size() < doc::kMaxEffectsPerChain);
  const bool racksShown = racksHere();
  if (style_.isVisible() != racksShown) { style_.setVisible(racksShown); rackButton_.setVisible(racksShown); fcoreButton_.setVisible(racksShown); duckButton_.setVisible(racksShown); duckDot_.setVisible(racksShown); resized(); }
  if (t && shape == shape_) {
    if (const auto f = doc::fcoreOf(*t); f && fcoreCard_) fcoreCard_->update(*f, "");
  }
  repaint();
}

void EffectsPanel::handleAsyncUpdate() {
  const doc::Document& d = host_.currentDocument();
  cards_.clear();
  shape_ = shapeOf();
  if (!trackId_.empty() && !d.findTrack(trackId_)) {  // the track went away
    if (!solo_.empty() && soloDone_) soloDone_(false);
    repaint();
    return;
  }
  const json chain = doc::effectChain(d, trackId_);  // a copy (see documentChanged)
  fcoreCard_.reset();
  if (const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_)) {
    if (const auto f = doc::fcoreOf(*t)) {
      fcoreCard_ = std::make_unique<FCoreCard>(*this, *f);
      strip_.addAndMakeVisible(*fcoreCard_);
    }
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    // FCore's effects show in its card; as their own cards only with Show inside.
    if (doc::isFCoreEffect(chain[i]) && fcoreCard_ && !expanded_.count(FCoreCard::kInside)) continue;
    cards_.push_back(std::make_unique<Card>(*this, (int)i, chain[i]));
    strip_.addAndMakeVisible(*cards_.back());
  }
  // The SIGNAL PATH: the instrument, F-Core, then the effects in the order they play.
  std::vector<ChainList::Row> rows;
  if (const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_)) {
    juce::String name = t->instrument.kind == "audio" ? u8("Audio") : u8(t->instrument.name);
    for (const auto& p : doc::corePlugins())
      if (p.kind == doc::CorePluginKind::Instrument && !p.instrument.empty() && p.instrument == t->instrument.name) name = u8(p.shortName);
    rows.push_back({ChainList::kInstrument, name, u8("Instrument"), true});
  }
  if (fcoreCard_) rows.push_back({"fcore", u8("F-Core · FX"), {}, true});
  for (const auto& c : cards_) rows.push_back({c->chainKey(), c->chainName(), {}, c->isOn()});
  list_->setRows(std::move(rows));
  if (!solo_.empty()) {  // an unpinned editor's window: only its card (the window goes when the effect left the chain)
    if (!soloCard() && soloDone_) soloDone_(false);
    resized();
    repaint();
    return;
  }
  // The chosen row: kept while it is in the chain; the effect just added; else the first effect.
  std::string& chosen = selected_[trackId_];
  if (scrollToEnd_ && !cards_.empty()) chosen = cards_.back()->chainKey();
  else if (chosen.empty() || chosen == ChainList::kInstrument || !list_->has(chosen))
    chosen = fcoreCard_ ? std::string("fcore") : cards_.empty() ? std::string() : cards_.front()->chainKey();
  tidyEditors();
  // The chosen effect's editor is open (the first time, or the effect just added).
  if (!chosen.empty() && chosen != ChainList::kInstrument) {
    auto& open = openList();
    const bool isNew = std::find(open.begin(), open.end(), chosen) == open.end();
    if (isNew && (open.empty() || scrollToEnd_)) open.push_back(chosen);
    if (scrollToEnd_) minimised_[trackId_].erase(chosen);
  }
  resized();
  if (scrollToEnd_ && knobs) view_.setViewPosition(0, strip_.getHeight());  // the effect just added (at the end)
  scrollToEnd_ = false;
  repaint();
}

std::vector<std::string> EffectsPanel::targets() const {
  if (trackId_.empty()) return {trackId_};
  const auto selected = host_.selectedTracks();
  if (std::find(selected.begin(), selected.end(), trackId_) != selected.end()) return selected;
  return {trackId_};
}

void EffectsPanel::addEffect(const std::string& name, int version) {
  const std::vector<std::string> tracks = targets();
  const doc::EffectInfo* info = doc::findEffect(name);
  host_.applyEdit([&](doc::Document& d) {
    for (const auto& target : tracks) {
      const int at = doc::addEffect(d, target, name);
      if (version > 0) doc::effectChain(d, target)[(size_t)at]["version"] = version;  // an older version, chosen
    }
  }, u8("Add ") + (info ? u8(info->label) : u8(name)) + (version > 0 ? " v" + juce::String(version) : juce::String()));
  scrollToEnd_ = true;  // once its card is built
}

int EffectsPanel::filterCount() const {
  const doc::Document& d = host_.currentDocument();
  int n = 0;
  if (!trackId_.empty() && !d.findTrack(trackId_)) return 0;
  for (const auto& fx : doc::effectChain(d, trackId_)) n += doc::effectName(fx) == "filter";
  return n;
}

// The filter library (Diogo 2026-11-07: "temos os vários filtros numa biblioteca... stack the filters"): each one
// is a Filter card in the row at the bottom; fold it to the side with ‹ to keep the one you are working on big.
void EffectsPanel::addFilter(int type) {
  if (filterCount() >= kMaxFilters) return;
  const std::vector<std::string> tracks = targets();
  host_.applyEdit([&](doc::Document& d) {
    for (const auto& target : tracks) {
      int n = 0;
      for (const auto& fx : doc::effectChain(d, target)) n += doc::effectName(fx) == "filter";
      if (n >= kMaxFilters) continue;
      const int at = doc::addEffect(d, target, "filter");
      doc::setEffectParam(d, target, at, "mode", type);
    }
  }, u8("Add filter"));
  scrollToEnd_ = true;
}

void EffectsPanel::setParam(int index, const std::string& param, double value) {
  const std::string target = trackId_;
  host_.applyEdit([&](doc::Document& d) { doc::setEffectParam(d, target, index, param, value); }, u8("Effect"));
}

void EffectsPanel::paint(juce::Graphics& g) {
  g.setColour(kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
  if (!solo_.empty()) return;  // an unpinned editor: its bar and its card
  if (chain()) {
    const juce::Colour ident = theme::identityOf(*this, theme::kBlueSoft);
    theme::paintSectionTitle(g, pathArea_.withHeight(20).toFloat(), u8("Signal path"), ident);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(lang::tr(trackId_.empty() ? u8("Output · the speakers") : u8("Output · Master")),
               pathArea_.withTop(pathArea_.getBottom() - 20), juce::Justification::centredLeft, true);
    if (!editorsHead_.isEmpty() && !picker_) {
      auto head = editorsHead_;
      g.setColour(theme::kTitle);
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      g.drawText("Bottom", head.removeFromLeft(64), juce::Justification::centredLeft, false);
      g.setColour(kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody));
      const int mini = minimisedEditors(), unpinned = unpinnedEditors();
      g.drawText(juce::String(openEditors()) + " open" + (mini > 0 ? u8(" \xc2\xb7 ") + juce::String(mini) + " minimised" : juce::String()) +
                     (unpinned > 0 ? u8(" \xc2\xb7 ") + juce::String(unpinned) + " unpinned" : juce::String()),
                 head, juce::Justification::centredLeft, false);
    }
  }
  if (chain() && !picker_ && (!cards_.empty() || fcoreCard_) && openEditors() == 0) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(lang::tr(minimisedEditors() > 0 ? u8("The editors are minimised at the side: click one to open it again.")
                                               : u8("Click an effect in the Signal path to open its editor here.")),
               view_.getBounds(), juce::Justification::centred);
  }
  if (cards_.empty() && !fcoreCard_ && !picker_) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(lang::tr(racksHere() ? u8("No effects. «+ Add effect» adds one effect; «Style FX» adds several at once, set for the style.")
                           : u8("No effects. Press «+ Add effect» to choose one of Core's own effects.")),
               (chain() ? view_.getBounds() : getLocalBounds().withTrimmedTop(40)), juce::Justification::centred);
  }
}

void EffectsPanel::resized() {
  if (!solo_.empty()) {  // an unpinned editor's window: its bar (▣) over its card, the whole window wide
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &hint_, &add_, &automation_, &attach_, &close_, &ab_, &style_,
                                                           &rackButton_, &fcoreButton_, &duckButton_, &duckDot_, &listView_})
      c->setVisible(false);
    pathArea_ = editorsHead_ = {};
    tabs_.clear();
    bars_.clear();
    auto r = getLocalBounds().reduced(8);
    juce::Component* card = soloCard();
    if (fcoreCard_) fcoreCard_->setVisible(fcoreCard_.get() == card);
    for (auto& c : cards_) c->setVisible(c.get() == card);
    if (!card) return;
    juce::String name = solo_ == "fcore" ? u8("F-Core") : juce::String();
    for (const auto& c : cards_) if (c.get() == card) name = c->chainName();
    const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
    Card* asCard = nullptr;
    for (auto& c : cards_) if (c.get() == card) asCard = c.get();
    auto bar = std::make_unique<EditorBar>(*this, solo_, name, t ? u8(t->name) : juce::String("Master"), true, asCard);
    bar->setBounds(r.removeFromTop(EditorBar::kHeight));
    addAndMakeVisible(*bar);
    bars_.push_back(std::move(bar));
    view_.setBounds(r);
    const int ch = card == fcoreCard_.get() ? fcoreCard_->height() : [&] { for (auto& c : cards_) if (c.get() == card) return c->height(); return 0; }();
    const int w = r.getWidth() - (ch > r.getHeight() ? view_.getScrollBarThickness() : 0);
    card->setBounds(0, 0, w, ch);
    strip_.setSize(w, ch);
    return;
  }
  auto r = getLocalBounds().reduced(10, 6);
  auto top = r.removeFromTop(28);
  close_.setVisible(onClose != nullptr);
  automation_.setVisible(onAutomate != nullptr);
  if (close_.isVisible()) close_.setBounds(top.removeFromRight(32).reduced(2, 0));
  minimise_.setVisible(onMinimise != nullptr);
  if (minimise_.isVisible()) {
    top.removeFromRight(2);
    minimise_.setBounds(top.removeFromRight(32).reduced(2, 0));
  }
  top.removeFromRight(6);
  ab_.setBounds(top.removeFromRight(AbSwitch::kWidth).reduced(0, 2));
  title_.setBounds(top.removeFromLeft(260));
  if (!chain()) {  // the Mixer's knob grid: + Effect and + Filter in the header
    add_.setBounds(top.removeFromLeft(120).reduced(2, 0));
  }
  if (rackButton_.isVisible()) {
    style_.setBounds(top.removeFromLeft(130).reduced(2, 1));
    rackButton_.setBounds(top.removeFromLeft(100).reduced(2, 0));
    fcoreButton_.setBounds(top.removeFromLeft(110).reduced(2, 0));
    duckButton_.setBounds(top.removeFromLeft(70).reduced(2, 0));
    duckDot_.setBounds(top.removeFromLeft(theme::InfoDot::kSlot + 2).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
  }
  if (automation_.isVisible()) automation_.setBounds(top.removeFromLeft(100).reduced(2, 0));
  attach_.setVisible(onAttachments != nullptr && !trackId_.empty());
  if (attach_.isVisible()) attach_.setBounds(top.removeFromLeft(110).reduced(2, 0));
  hint_.setBounds(top);
  r.removeFromTop(6);
  listView_.setVisible(chain());
  add_.setToggleState(picker_ != nullptr, juce::dontSendNotification);
  if (chain()) {
    // The SIGNAL PATH on the left, the chosen effect's card on the right.
    auto left = r.removeFromLeft(kPathWidth);
    r.removeFromLeft(12);
    pathArea_ = left;
    left.removeFromTop(26);  // SIGNAL PATH
    left.removeFromBottom(22);  // Output
    auto buttons = left.removeFromBottom(30);
    left.removeFromBottom(6);
    listView_.setBounds(left.withHeight(std::min(left.getHeight(), list_->height())));
    list_->setSize(listView_.getWidth() - (list_->height() > left.getHeight() ? listView_.getScrollBarThickness() : 0), list_->height());
    add_.setBounds(buttons.removeFromTop(28));
    // Over the editors: where they are ("Bottom") and how many are open or minimised.
    editorsHead_ = r.removeFromTop(22);
    r.removeFromTop(4);
    // The minimised editors: vertical tabs on the right edge.
    tabs_.clear();
    const auto& open = openList();
    const auto& mini = minimised_[trackId_];
    if (!mini.empty()) {
      auto side = r.removeFromRight(30);
      r.removeFromRight(6);
      for (const auto& k : open) {
        if (!mini.count(k)) continue;
        juce::String name = k == "fcore" ? u8("F-Core") : juce::String();
        for (const auto& c : cards_) if (c->chainKey() == k) name = c->chainName();
        auto tab = std::make_unique<SideTab>(*this, k, name);
        addAndMakeVisible(*tab);
        tab->setBounds(side.removeFromTop(std::min(190, std::max(120, side.getHeight() / std::max(1, (int)mini.size())))));
        side.removeFromTop(4);
        tabs_.push_back(std::move(tab));
      }
    }
    view_.setBounds(r);
    for (auto& b : bars_) strip_.removeChildComponent(b.get());
    bars_.clear();
    if (layoutPicker(r)) return;
    // The open editors side by side, each at its own width.
    std::vector<std::pair<std::string, juce::Component*>> shown;
    for (const auto& k : open) {
      if (mini.count(k)) continue;
      juce::Component* c = k == "fcore" ? static_cast<juce::Component*>(fcoreCard_.get()) : nullptr;
      for (auto& cc : cards_) if (cc->chainKey() == k) c = cc.get();
      if (c) shown.push_back({k, c});
    }
    if (fcoreCard_) fcoreCard_->setVisible(false);
    for (auto& c : cards_) c->setVisible(false);
    const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
    const juce::String trackName = t ? u8(t->name) : juce::String("Master");
    int x = 0, h = r.getHeight() - 12;
    for (const auto& [k, c] : shown) {
      const bool isF = c == fcoreCard_.get();
      const int w = isF ? kRackCardWidth : kKnobCardWidth;
      const int ch = isF ? fcoreCard_->height() : [&] { for (auto& cc : cards_) if (cc.get() == c) return cc->height(); return 0; }();
      juce::String name = isF ? u8("F-Core") : juce::String();
      for (const auto& cc : cards_) if (cc.get() == c) name = cc->chainName();
      Card* asCard = nullptr;
      for (auto& cc : cards_) if (cc.get() == c) asCard = cc.get();
      auto bar = std::make_unique<EditorBar>(*this, k, name, trackName, false, asCard);
      bar->setBounds(x, 0, w, EditorBar::kHeight);
      strip_.addAndMakeVisible(*bar);
      bars_.push_back(std::move(bar));
      c->setVisible(true);
      c->setBounds(x, EditorBar::kHeight, w, ch);
      x += w + 8;
      h = std::max(h, EditorBar::kHeight + ch);
    }
    strip_.setSize(std::max(x, r.getWidth() - 12), h);
    return;
  }
  view_.setBounds(r);
  if (layoutPicker(r)) return;
  // FCore's card first, then the plain effects in chain order.
  std::vector<std::pair<juce::Component*, int>> all;
  std::vector<int> widths;
  if (fcoreCard_) { all.push_back({fcoreCard_.get(), fcoreCard_->height()}); widths.push_back(kRackCardWidth); }
  for (auto& c : cards_) { c->setVisible(true); all.push_back({c.get(), c->height()}); widths.push_back(kKnobCardWidth); }
  if (knobs) {  // knob cards fill columns (each card under the shortest column), and scroll down
    const int columns = std::max(1, (r.getWidth() - 12 + 8) / (kKnobCardWidth + 8));
    const int width = std::max(kKnobCardWidth, (r.getWidth() - 12 - (columns - 1) * 8) / columns);
    std::vector<int> heights((size_t)columns, 0);
    for (auto& [c, height] : all) {
      const size_t col = (size_t)(std::min_element(heights.begin(), heights.end()) - heights.begin());
      c->setBounds((int)col * (width + 8), heights[col], width, height);
      heights[col] += height + 8;
    }
    strip_.setSize(std::max(r.getWidth() - 12, columns * (width + 8) - 8), std::max(*std::max_element(heights.begin(), heights.end()), r.getHeight() - 12));
    return;
  }
  int x = 0, h = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    const int width = widths[i];
    all[i].first->setBounds(x, 0, width, all[i].second);
    x += width + 8;
    h = std::max(h, all[i].second);
  }
  strip_.setSize(std::max(x, r.getWidth()), std::max(h, r.getHeight() - 12));
}

} // namespace core::app
