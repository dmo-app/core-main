#include "ChainsView.h"
#include "Theme.h"
#include "TrackColour.h"
#include "core_plugins.hpp"
#include "edit.hpp"
#include "fcore.hpp"

namespace core::app {

namespace {
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
}  // namespace

class ChainsView::Content final : public juce::Component {
public:
  explicit Content(ChainsView& o) : owner_(o) {}
  struct Row { std::string key; juce::String name; bool on; };
  struct Column { std::string id; juce::String name, instrument; juce::Colour colour; std::vector<Row> effects; };
  std::vector<Column> columns;
  std::vector<Hit> hits;
  std::string hover;

  int columnHeight(const Column& c) const { return 34 + 8 + ChainsView::kRow + 6 + (int)c.effects.size() * (ChainsView::kRow + 4) + ChainsView::kRow + 30; }
  void layout(int height) {
    int h = height;
    for (const auto& c : columns) h = std::max(h, columnHeight(c) + 16);
    setSize(std::max(owner_.view_.getMaximumVisibleWidth(), (int)columns.size() * (ChainsView::kColumn + ChainsView::kGap) + 8), h);
  }
  void paint(juce::Graphics& g) override {
    g.fillAll(theme::kBackground);
    hits.clear();
    int x = 8;
    int number = 0;
    for (const auto& c : columns) {
      ++number;
      auto col = juce::Rectangle<int>(x, 8, ChainsView::kColumn, getHeight() - 16);
      x += ChainsView::kColumn + ChainsView::kGap;
      const bool chosen = c.id == owner_.selected_;
      g.setColour(theme::kPanel);
      g.fillRect(col);
      g.setColour(chosen ? c.colour : theme::kOutline);
      g.drawRect(col, chosen ? 2 : 1);
      auto r = col.reduced(8, 6);
      // The header: its number and its name in the track's colour.
      auto head = r.removeFromTop(28);
      hits.push_back({head, c.id, {}, 0});
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String(number).paddedLeft('0', 2), head.removeFromLeft(26), juce::Justification::centredLeft);
      const juce::Font nameFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
      const int nw = std::min(head.getWidth(), juce::GlyphArrangement::getStringWidthInt(nameFont, c.name) + 16);
      auto chip = head.removeFromRight(nw);
      g.setColour(c.colour.withAlpha(0.85f));
      g.fillRect(chip);
      g.setColour(c.colour.getPerceivedBrightness() > 0.55f ? theme::kBackground : theme::kText);
      g.setFont(nameFont);
      g.drawText(c.name, chip, juce::Justification::centred, true);
      r.removeFromTop(8);
      auto slot = [&](juce::Rectangle<int> s, const juce::String& text, bool on, bool dot, const std::string& key, int what, bool muted) {
        const bool over = hover == c.id + "|" + key + "|" + std::to_string(what);
        g.setColour(over ? theme::kControlHover : theme::kControl);
        g.fillRect(s);
        g.setColour(theme::kOutline);
        g.drawRect(s);
        auto t = s.reduced(8, 0);
        if (dot) {
          const auto d = juce::Rectangle<float>(9.0f, 9.0f).withCentre({(float)t.getX() + 4.5f, (float)t.getCentreY()});
          g.setColour(c.colour);
          if (on) g.fillEllipse(d); else g.drawEllipse(d, 1.2f);
          t.removeFromLeft(16);
        }
        g.setColour(muted ? theme::kMuted : theme::kText);
        g.setFont(juce::FontOptions(theme::kFontBody));
        g.drawText(text, t, dot ? juce::Justification::centredLeft : juce::Justification::centred, true);
        hits.push_back({s, c.id, key, what});
      };
      // The instrument, then the effects in the order they play.
      if (c.instrument.isNotEmpty()) {
        slot(r.removeFromTop(ChainsView::kRow), c.instrument, true, true, "instrument", 1, false);
        r.removeFromTop(6);
      }
      for (const auto& e : c.effects) {
        slot(r.removeFromTop(ChainsView::kRow), e.name, e.on, true, e.key, 2, !e.on);
        r.removeFromTop(4);
      }
      if (c.effects.empty() && c.instrument.isEmpty()) {
        g.setColour(theme::kMuted);
        g.setFont(juce::FontOptions(theme::kFontSmall));
        g.drawText("No devices", r.removeFromTop(22), juce::Justification::centredLeft);
      }
      slot(r.removeFromTop(ChainsView::kRow), "+ Add effect", true, false, "add", 3, false);
      r.removeFromTop(8);
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String::fromUTF8("Output \xe2\x86\x92 Master"), r.removeFromTop(18), juce::Justification::centredLeft);
    }
    if (columns.empty()) {
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawText("No tracks yet.", getLocalBounds(), juce::Justification::centred);
    }
  }
  void mouseMove(const juce::MouseEvent& e) override {
    std::string now;
    for (const auto& h : hits) if (h.r.contains(e.getPosition()) && h.what > 0) now = h.track + "|" + h.key + "|" + std::to_string(h.what);
    if (now != hover) { hover = now; repaint(); }
  }
  void mouseExit(const juce::MouseEvent&) override { if (!hover.empty()) { hover.clear(); repaint(); } }
  void mouseUp(const juce::MouseEvent& e) override {
    for (const auto& h : hits) {
      if (!h.r.contains(e.getPosition())) continue;
      // After the click: what it opens may rebuild this view.
      juce::Component::SafePointer<ChainsView> safe(&owner_);
      const Hit hit = h;
      juce::MessageManager::callAsync([safe, hit] {
        if (!safe) return;
        auto& o = *safe;
        if (hit.what == 0 && o.onSelectTrack) o.onSelectTrack(hit.track);
        if (hit.what == 1 && o.onOpenInstrument) o.onOpenInstrument(hit.track);
        if (hit.what == 2 && o.onOpenEffect) o.onOpenEffect(hit.track, hit.key);
        if (hit.what == 3 && o.onAddEffect) o.onAddEffect(hit.track);
      });
      return;
    }
  }

private:
  ChainsView& owner_;
};

ChainsView::ChainsView(PatternEditor::Host& host) : host_(host) {
  content_ = std::make_unique<Content>(*this);
  view_.setViewedComponent(content_.get(), false);
  view_.setScrollBarsShown(true, true);
  addAndMakeVisible(view_);
  setOpaque(true);
}

ChainsView::~ChainsView() { view_.setViewedComponent(nullptr, false); }

void ChainsView::setSelected(const std::string& trackId) {
  if (trackId == selected_) return;
  selected_ = trackId;
  content_->repaint();
}

void ChainsView::documentChanged() {
  const doc::Document& d = host_.currentDocument();
  content_->columns.clear();
  for (const auto& t : d.tracks) {
    Content::Column c;
    c.id = t.id;
    c.name = u8(t.name);
    c.colour = trackColour(t);
    if (t.instrument.kind == "builtin") {
      c.instrument = u8(t.instrument.name);
      for (const auto& p : doc::corePlugins())
        if (p.kind == doc::CorePluginKind::Instrument && p.instrument == t.instrument.name) c.instrument = u8(p.shortName);
    } else if (t.instrument.kind == "audio") {
      c.instrument = "Audio";
    }
    try {
      const auto chain = doc::effectChain(d, t.id);
      if (doc::fcoreOf(t)) c.effects.push_back({"fcore", juce::String::fromUTF8("F-Core \xc2\xb7 FX"), true});
      for (size_t i = 0; i < chain.size(); ++i) {
        const auto& fx = chain[i];
        if (doc::isFCoreEffect(fx)) continue;
        const std::string name = doc::effectName(fx);
        const doc::EffectInfo* info = doc::findEffect(name);
        const std::string id = fx.is_object() ? fx.value("id", "") : std::string();
        c.effects.push_back({id.empty() ? "#" + std::to_string(i) : id, info ? u8(info->label) : u8(name), !doc::effectBypassed(fx)});
      }
    } catch (const std::exception&) {}
    content_->columns.push_back(std::move(c));
  }
  content_->layout(view_.getMaximumVisibleHeight());
  content_->repaint();
}

void ChainsView::resized() {
  view_.setBounds(getLocalBounds());
  content_->layout(view_.getMaximumVisibleHeight());
}

}  // namespace core::app
