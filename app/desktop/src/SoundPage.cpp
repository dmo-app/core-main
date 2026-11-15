#include "SoundPage.h"
#include <algorithm>
#include "Theme.h"
#include "ModulationLive.h"
#include "edit.hpp"
#include <cmath>
#include <vector>
#include "../../../engine/src/dsp/voice_filter.hpp"

namespace core::app {
namespace {
juce::String u8(const char* t) { return juce::String::fromUTF8(t); }

// The filter's curve as the engine's own VoiceFilter answers (dsp::filterCurveDb: measured, exact), in dB at each
// of the frequencies. The kick's and the Sampler's Filter: 1 Low, 2 Band, 3 High (0 Off: flat).
constexpr int kCurvePoints = 160;  // across 20 Hz .. 20 kHz; ~1.3 M multiply-adds per curve, only when it changes
std::vector<double> curveDb(int type, double fc, double resonance) {
  static const std::vector<double> hz = [] {
    std::vector<double> f(kCurvePoints + 1);
    for (int i = 0; i <= kCurvePoints; ++i) f[(size_t)i] = 20.0 * std::pow(1000.0, (double)i / kCurvePoints);
    return f;
  }();
  std::vector<double> db(hz.size(), 0.0);
  if (type <= 0) return db;
  const int voice = type == 1 ? dsp::VoiceFilter::Low12 : type == 2 ? dsp::VoiceFilter::Band : dsp::VoiceFilter::High12;
  dsp::filterCurveDb(voice, std::clamp(fc, 20.0, 20000.0), std::clamp(resonance, 0.0, 0.95), 0.0, 0.5, 48000.0, hz.data(), db.data(), (int)hz.size());
  return db;
}
}  // namespace

// The track's filter as a curve: where it cuts now (bright, moving with the LFO / envelope / steps on Cutoff and
// Resonance, and with the instrument's own Filter env after each note) over where it is set (faint).
class SoundPage::FilterView final : public juce::Component, private juce::Timer {
public:
  explicit FilterView(PatternEditor::Host& host) : host_(host) { setInterceptsMouseClicks(false, false); }
  void setTrack(const std::string& id) { trackId_ = id; update(); }
  bool has() const {
    const doc::Track* t = track();
    return t && doc::findInstrumentParam(*t, "cutoff") && doc::findInstrumentParam(*t, "filterType");
  }
  void visibilityChanged() override { if (isVisible()) startTimerHz(30); else stopTimer(); }
  void timerCallback() override { if (isShowing()) update(); }

  void paint(juce::Graphics& g) override {
    using namespace theme;
    auto area = getLocalBounds().toFloat();
    g.setColour(kField);
    g.fillRoundedRectangle(area, theme::kCorner);
    g.setColour(kOutline);
    g.drawRoundedRectangle(area.reduced(0.5f), theme::kCorner, 1);
    auto plot = area.reduced(8, 18);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.setColour(kDim);
    g.drawText(juce::String::fromUTF8("FILTER"), area.reduced(8, 3).removeFromTop(14), juce::Justification::topLeft);
    const char* names[] = {"Off", "Low", "Band", "High"};
    g.drawText(juce::String(names[std::clamp(type_, 0, 3)]) + "  " + hz(liveCut_), area.reduced(8, 3).removeFromTop(14),
               juce::Justification::topRight);
    // 20 Hz .. 20 kHz across, -30 .. +18 dB up.
    auto xOf = [&](double f) { return plot.getX() + plot.getWidth() * (float)(std::log(f / 20.0) / std::log(1000.0)); };
    auto yOf = [&](double db) { return plot.getY() + plot.getHeight() * (float)((18.0 - std::clamp(db, -30.0, 18.0)) / 48.0); };
    g.setColour(kOutline.withAlpha(0.6f));
    for (double f : {100.0, 1000.0, 10000.0}) g.fillRect(xOf(f), plot.getY(), 1.0f, plot.getHeight());
    g.fillRect(plot.getX(), yOf(0), plot.getWidth(), 1.0f);
    g.setColour(kDim);
    for (auto [f, t] : {std::pair{100.0, "100"}, std::pair{1000.0, "1k"}, std::pair{10000.0, "10k"}})
      g.drawText(t, juce::Rectangle<float>(xOf(f) + 2, plot.getBottom() + 1, 30, 12), juce::Justification::centredLeft);
    auto curve = [&](const std::vector<double>& db, bool fill) {
      juce::Path p;
      if (db.size() < 2) return p;
      const int n = (int)db.size() - 1;
      for (int i = 0; i <= n; ++i) {
        const float x = plot.getX() + plot.getWidth() * (float)i / (float)n, y = yOf(db[(size_t)i]);
        if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
      }
      if (fill) {
        juce::Path f = p;
        f.lineTo(plot.getRight(), plot.getBottom());
        f.lineTo(plot.getX(), plot.getBottom());
        f.closeSubPath();
        g.setColour(kAccent.withAlpha(0.18f));
        g.fillPath(f);
      }
      return p;
    };
    g.setColour(kMuted.withAlpha(0.45f));
    g.strokePath(curve(baseDb_, false), juce::PathStrokeType(1.2f));  // where it is set
    const auto now = curve(liveDb_, true);
    g.setColour(kAccentBright);
    g.strokePath(now, juce::PathStrokeType(2.0f));  // where it is now
    if (type_ > 0) {
      g.setColour(kAccentBright.withAlpha(0.5f));
      g.fillRect(xOf(std::clamp(liveCut_, 20.0, 20000.0)), plot.getY(), 1.0f, plot.getHeight());
    }
  }

private:
  const doc::Track* track() const { return trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_); }
  static juce::String hz(double v) { return v >= 1000 ? juce::String(v / 1000.0, 1) + " kHz" : juce::String((int)std::lround(v)) + " Hz"; }
  void update() {
    const doc::Document& d = host_.currentDocument();
    const doc::Track* t = track();
    if (!t) return;
    auto param = [&](const char* name) { return doc::findInstrumentParam(*t, name); };
    const auto* cut = param("cutoff");
    const auto* res = param("resonance");
    const auto* type = param("filterType");
    if (!cut || !type) return;
    const double tick = host_.playheadTick();
    const int ty = (int)std::lround(doc::instrumentParamValue(*t, *type));
    const double bc = doc::instrumentParamValue(*t, *cut), br = res ? doc::instrumentParamValue(*t, *res) : 0.1;
    double lc = live::value(d, *t, *cut, tick), lr = res ? live::value(d, *t, *res, tick) : br;
    // The instrument's own Filter env: up (or down) that many octaves at each note, back over Filter decay.
    if (const auto* fe = param("filterEnv")) {
      const double oct = doc::instrumentParamValue(*t, *fe);
      const auto* fd = param("filterDecay");
      const double note = live::lastNoteTick(*t, tick);
      if (std::fabs(oct) > 1e-6 && note >= 0) {
        const double ms = (tick - note) / doc::kPpq * 60000.0 / std::max(1.0, doc::bpmAt(d, (int64_t)note));
        const double decay = fd ? std::clamp(doc::instrumentParamValue(*t, *fd), 5.0, 4000.0) : 200.0;
        lc = std::clamp(lc * std::exp2(oct * std::pow(0.001, ms / decay)), 20.0, 20000.0);
      }
    }
    if (ty == type_ && std::fabs(bc - baseCut_) < 1e-3 && std::fabs(br - baseRes_) < 1e-4 && std::fabs(lc - liveCut_) < 0.05 && std::fabs(lr - liveRes_) < 1e-4) return;
    // The curves are measured on the filter itself: again only when what they show changed.
    if (ty != type_ || bc != baseCut_ || br != baseRes_ || baseDb_.empty()) baseDb_ = curveDb(ty, bc, br);
    if (ty != type_ || lc != liveCut_ || lr != liveRes_ || liveDb_.empty()) liveDb_ = curveDb(ty, lc, lr);
    type_ = ty; baseCut_ = bc; baseRes_ = br; liveCut_ = lc; liveRes_ = lr;
    repaint();
  }
  PatternEditor::Host& host_;
  std::string trackId_;
  int type_ = -1;
  double baseCut_ = 20000, baseRes_ = 0.1, liveCut_ = 20000, liveRes_ = 0.1;
  std::vector<double> baseDb_, liveDb_;  // the curves (dB at kCurvePoints + 1 frequencies)
};

SoundPage::~SoundPage() = default;

SoundPage::SoundPage(PatternEditor::Host& host) : host_(host), sound_(host), modulation_(host) {
  title_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
  title_.setColour(juce::Label::textColourId, theme::kText);
  addAndMakeVisible(title_);
  close_.setButtonText(u8("✕"));
  close_.setTooltip(u8("Close (back to the piano roll)"));
  close_.setWantsKeyboardFocus(false);
  close_.onClick = [this] { if (onClose) onClose(); };
  addAndMakeVisible(close_);
  sound_.onModulate = [this](const std::string& param, int kind) { modulate(param, kind); };
  sound_.onModulatorDropped = [this](const std::string& id, const std::string& param) { modulation_.setDestination(id, param); };
  addAndMakeVisible(sound_);
  addAndMakeVisible(modulation_);
  filter_ = std::make_unique<FilterView>(host);
  addChildComponent(*filter_);
}

void SoundPage::setTarget(const std::string& trackId) {
  trackId_ = trackId;
  sound_.setTrack(trackId);
  modulation_.setTarget(trackId);
  filter_->setTrack(trackId);
  documentChanged();
}

void SoundPage::documentChanged() {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  title_.setText(t ? juce::String::fromUTF8(t->name.c_str()).toUpperCase() : juce::String(), juce::dontSendNotification);
  sound_.documentChanged();
  modulation_.documentChanged();
  if (filter_->has() != filter_->isVisible()) { filter_->setVisible(filter_->has()); resized(); }
  repaint();
}

void SoundPage::modulate(const std::string& param, int kind) {
  modulation_.modulate(kind == 1 ? ModulationPanel::Mode::Env : kind == 2 ? ModulationPanel::Mode::Step : ModulationPanel::Mode::Lfo, param);
}

void SoundPage::paint(juce::Graphics& g) {
  g.setColour(theme::kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
}

void SoundPage::resized() {
  auto r = getLocalBounds().reduced(8, 4);
  auto head = r.removeFromTop(30);
  close_.setBounds(head.removeFromRight(28).reduced(2));
  title_.setBounds(head);
  r.removeFromTop(4);
  // The bars on top (taller than on the card: easier to drag), the modulation right under them.
  const int modH = std::max(ModulationPanel::kCompactHeight, std::min(200, r.getHeight() / 2));
  modulation_.setBounds(r.removeFromBottom(modH));
  r.removeFromBottom(8);
  r = r.withHeight(std::min(r.getHeight(), 240));
  // The filter's curve at the right of the bars, when the instrument has a filter.
  if (filter_->isVisible()) {
    const int w = std::clamp(r.getWidth() - sound_.preferredWidth() - 8, 220, 360);
    filter_->setBounds(r.removeFromRight(w));
    r.removeFromRight(8);
  }
  sound_.setBounds(r);
}

}  // namespace core::app
