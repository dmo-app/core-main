// Core EQ in its own window (2026-11-09), in the style of the Core Frequency FX window (Diogo, 2026-11-08 23:52):
// the bands as hills over the input spectrum, each in its own blue from light to dark (frost, sky, petrol, navy),
// a chip per band, the chosen band's values in one row, a card per band that is on, the LFOs and the output.
//
// The graph: drag a point (frequency left-right, gain up-down; a cut or a notch only moves left-right), the wheel
// over a point changes its Q, double-click an empty place adds a bell there, double-click a point turns its band
// off, right-click a point: shape, slope, channel, dynamic. Every change is one undo step of the song.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include "CoreFxPane.h"
#include "TrackColour.h"

namespace core::app {

using namespace corefx;

namespace {
using theme::kPanel, theme::kCard, theme::kField, theme::kText, theme::kMuted, theme::kDim, theme::kOutline, theme::kAccent;

constexpr int kBands = 8;
constexpr double kRange = 24;     // the graph: +-24 dB
constexpr double kDrawRate = 48000;
enum Shape { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch };

// One vivid colour per band, all different (Diogo, 2026-11-09).
constexpr int kShades = 8;
const juce::uint32 kColours[kShades] = {0xffff4d6d, 0xffff9f1c, 0xffffe14d, 0xff3ddc84,
                                      0xff00d1ff, 0xff3d7bff, 0xffa05cff, 0xffff5cd6};
juce::Colour bandColour(int b) { return juce::Colour(kColours[b % kShades]); }
juce::Colour lineColour(int b) { return bandColour(b); }
juce::Colour inkOn(int b) { return bandColour(b).getPerceivedBrightness() > 0.55f ? theme::kBackground : theme::kText; }

std::string key(int b, const char* k) { return "b" + std::to_string(b + 1) + k; }

bool isCut(int shape) { return shape == LowCut || shape == HighCut; }
bool gained(int shape) { return shape == Bell || shape == LowShelf || shape == HighShelf; }

const char* shapeName(int shape) {
  static const char* n[] = {"Bell", "Low shelf", "High shelf", "Low cut", "High cut", "Notch"};
  return n[std::clamp(shape, 0, 5)];
}
const char* regionName(double f) {
  if (f < 60) return "Sub";
  if (f < 250) return "Low";
  if (f < 800) return "Low Mid";
  if (f < 2500) return "Mid";
  if (f < 6000) return "High Mid";
  if (f < 12000) return "Presence";
  return "Air";
}

// The engine's filters (effects.cpp Biquad, RBJ), as magnitudes in dB at a frequency.
struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
Coeffs design(int kind, double freq, double q, double gainDb) {  // kind: 0 LP, 1 HP, 3 Peak, 4 LS, 5 HS, 6 Notch
  freq = std::clamp(freq, 10.0, kDrawRate * 0.49);
  const double w = 2 * juce::MathConstants<double>::pi * freq / kDrawRate, cw = std::cos(w), sw = std::sin(w);
  const double alpha = sw / (2 * std::max(0.05, q)), A = std::pow(10.0, gainDb / 40.0);
  double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
  switch (kind) {
    case 0: nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = nb0; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
    case 1: nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = nb0; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
    case 6: nb0 = 1; nb1 = -2 * cw; nb2 = 1; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
    case 3: nb0 = 1 + alpha * A; nb1 = -2 * cw; nb2 = 1 - alpha * A; na0 = 1 + alpha / A; na1 = -2 * cw; na2 = 1 - alpha / A; break;
    case 4: {
      const double s = 2 * std::sqrt(A) * alpha;
      nb0 = A * ((A + 1) - (A - 1) * cw + s); nb1 = 2 * A * ((A - 1) - (A + 1) * cw); nb2 = A * ((A + 1) - (A - 1) * cw - s);
      na0 = (A + 1) + (A - 1) * cw + s; na1 = -2 * ((A - 1) + (A + 1) * cw); na2 = (A + 1) + (A - 1) * cw - s;
      break;
    }
    default: {
      const double s = 2 * std::sqrt(A) * alpha;
      nb0 = A * ((A + 1) + (A - 1) * cw + s); nb1 = -2 * A * ((A - 1) + (A + 1) * cw); nb2 = A * ((A + 1) + (A - 1) * cw - s);
      na0 = (A + 1) - (A - 1) * cw + s; na1 = 2 * ((A - 1) - (A + 1) * cw); na2 = (A + 1) - (A - 1) * cw - s;
      break;
    }
  }
  return {nb0 / na0, nb1 / na0, nb2 / na0, na1 / na0, na2 / na0};
}
double magnitudeDb(const Coeffs& c, double freq) {
  const double w = 2 * juce::MathConstants<double>::pi * freq / kDrawRate;
  const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
  const std::complex<double> h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
  return 20 * std::log10(std::max(1e-9, std::abs(h)));
}

// One band as the engine has it.
struct BandView {
  bool on = false, dyn = false;
  int shape = Bell, slope = 0, chan = 0;
  double freq = 1000, gain = 0, q = 1, thr = -24, range = 0;
  // Its response in dB at a frequency, at a gain (the dynamic band's other end: gain + range).
  double response(double f, double atGain) const {
    if (isCut(shape)) {
      static const double q24[2] = {0.5411961, 1.3065630}, q48[4] = {0.5097956, 0.6013449, 0.8999762, 2.5629154};
      const int stages = slope == 0 ? 1 : slope == 1 ? 2 : 4;
      double db = 0;
      for (int s = 0; s < stages; ++s)
        db += magnitudeDb(design(shape == LowCut ? 1 : 0, freq, stages == 1 ? q : stages == 2 ? q24[s] : q48[s], 0), f);
      return db;
    }
    const int kind = shape == Bell ? 3 : shape == LowShelf ? 4 : shape == HighShelf ? 5 : 6;
    return magnitudeDb(design(kind, freq, q, atGain), f);
  }
  // Does it change the sound (the engine skips a neutral band).
  bool does() const {
    if (!on) return false;
    if (gained(shape)) return gain != 0 || (dyn && range != 0);
    if (shape == LowCut) return freq > 20.5;
    if (shape == HighCut) return freq < 19999.5;
    return true;
  }
};

// On: a circle in the band's dark tone, full when the band is on, empty when it is off.
class OnDot final : public juce::Component, public juce::SettableTooltipClient {
  public:
  OnDot(juce::Colour c, const juce::String& tip) : c_(c) { setTooltip(tip); setMouseCursor(juce::MouseCursor::PointingHandCursor); }
  void set(bool on) { if (on != on_) { on_ = on; repaint(); } }
  void tint(juce::Colour c) { if (c != c_) { c_ = c; repaint(); } }
  std::function<void()> onClick;
  void paint(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre(14, 14);
    // On / off: a lighter circle, full when on, empty when off (Diogo, 2026-11-09).
    const juce::Colour light = c_.interpolatedWith(juce::Colours::white, 0.35f);
    if (on_) { g.setColour(light); g.fillEllipse(r); }
    g.setColour(light);
    g.drawEllipse(r.reduced(0.75f), 1.5f);
  }
  void mouseUp(const juce::MouseEvent& e) override { if (e.mouseWasClicked() && onClick) onClick(); }
  private:
  juce::Colour c_;
  bool on_ = false;
};

// A / B as one switch: two halves, the one in use filled with the colour (Diogo, 2026-11-09).
class ABSwitch final : public juce::Component, public juce::SettableTooltipClient {
public:
  std::function<void(int)> onChoose;
  void set(int slot, juce::Colour c) { slot_ = slot; c_ = c; repaint(); }
  void paint(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    for (int i = 0; i < 2; ++i) {
      const auto half = i == 0 ? r.withWidth(r.getWidth() / 2) : r.withTrimmedLeft(r.getWidth() / 2);
      const bool chosen = i == slot_;
      g.setColour(chosen ? c_ : c_.withAlpha(0.22f));
      g.fillRect(half);
      g.setColour(chosen ? (c_.getPerceivedBrightness() > 0.62f ? theme::kBackground : juce::Colours::white) : theme::kText);
      g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
      g.drawText(i == 0 ? "A" : "B", half, juce::Justification::centred, false);
    }
    g.setColour(c_);
    g.drawRect(r, 1.5f);
  }
  void mouseUp(const juce::MouseEvent& e) override {
    if (e.mouseWasClicked() && onChoose) onChoose(e.x < getWidth() / 2 ? 0 : 1);
  }

private:
  int slot_ = 0;
  juce::Colour c_{0xff8693b6};
};
}  // namespace

// ---------------------------------------------------------------- the editor
class CoreEqEditor final : public CoreFxWindows::Pane {
public:
  class Graph;
  class Card;
  class Table;
  CoreEqEditor(CoreFxWindows& owner, std::string trackId, std::string fxId);

  juce::String title() const override { return u8("Core EQ"); }
  void refresh() override;
  void tick() override { for (auto& l : lfos_) l->tick(); }
  void spectrumChanged() override;
  void destinations(juce::PopupMenu& menu, const std::function<void(juce::PopupMenu&, const std::string&)>& add) override {
    add(menu, "output");
    for (int b = 0; b < kBands; ++b) {
      if (!band(b).on) continue;
      juce::PopupMenu sub;
      for (const char* k : {"Freq", "Gain", "Q", "Thr", "Range"}) add(sub, key(b, k));
      menu.addSubMenu(bandTitle(b), sub);
    }
  }

  juce::String destinationName(const std::string& param) const override {  // "b3Gain" -> "03 High Mid · Gain"
    if (param.size() > 2 && param[0] == 'b' && std::isdigit((unsigned char)param[1])) {
      const int b = param[1] - '1';
      const std::string what = param.substr(2);
      const char* word = what == "Freq" ? "Frequency" : what == "Thr" ? "Threshold" : what.c_str();
      if (b >= 0 && b < kBands)
        return (b < 9 ? u8("0") : u8("")) + juce::String(b + 1) + u8(" ") + u8(regionName(band(b).freq)) + u8(" · ") + u8(word);
    }
    return Pane::destinationName(param);
  }

  BandView band(int b) const {
    BandView v;
    v.on = get(key(b, "On")) > 0.5;
    v.shape = std::clamp((int)std::lround(get(key(b, "Type"))), 0, 5);
    v.freq = get(key(b, "Freq"));
    v.gain = get(key(b, "Gain"));
    v.q = get(key(b, "Q"));
    v.slope = std::clamp((int)std::lround(get(key(b, "Slope"))), 0, 2);
    v.chan = std::clamp((int)std::lround(get(key(b, "Chan"))), 0, 4);
    v.dyn = get(key(b, "Dyn")) > 0.5 && gained(v.shape);
    v.thr = get(key(b, "Thr"));
    v.range = get(key(b, "Range"));
    return v;
  }
  juce::String bandTitle(int b) const {
    const BandView v = band(b);
    return (b < 9 ? u8("0") : u8("")) + juce::String(b + 1) + u8("  ") + u8(regionName(v.freq)) + u8(" · ") + u8(shapeName(v.shape));
  }
  int selected() const { return selected_; }
  void select(int b);
  // A minimised card leaves the row and becomes a line of the table at the side (Diogo, 2026-11-09).
  bool folded(int b) const { return folded_[(size_t)b]; }
  void fold(int b, bool f) { folded_[(size_t)b] = f; refresh(); }
  void addBand(double freq, double gain);
  void duplicate();
  void resetAll();

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void layoutCards(juce::Rectangle<int> r);
  void bindSelected();

  int selected_ = 2;
  // The top bar.
  juce::Label where_;
  juce::ComboBox preset_;
  ABSwitch ab_;
  juce::TextButton copy_;
  juce::Colour tint_{0xff8693b6};  // the track's colour: the title panel, A/B, Copy and On
  juce::Rectangle<int> headArea_;
  OnDot on_{juce::Colour(0xff8a96b4), corefx::u8("The EQ on or off (off: the sound as it comes in)")};
  // The bands' bar.
  juce::Label kindLabel_;
  juce::TextButton autoGain_, addBand_;
  std::unique_ptr<Graph> graph_;
  std::array<juce::TextButton, kBands> chips_;
  // The chosen band.
  juce::Label selName_, freqLabel_, qLabel_, gainLabel_, thrLabel_, rangeLabel_;
  juce::Slider freq_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft}, q_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft},
      gain_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft}, thr_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft},
      range_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
  juce::TextButton dyn_;
  std::array<std::unique_ptr<Card>, kBands> cards_;
  std::array<bool, kBands> folded_{};
  std::unique_ptr<Table> table_;
  std::array<std::unique_ptr<CoreFxWindows::Lfo>, kLfos> lfos_;
  // The output.
  juce::Label outLabel_, analyzer_;
  juce::Slider output_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
  std::vector<const doc::EffectPreset*> presets_;
  juce::Rectangle<int> topArea_, bandsArea_, selArea_, cardsArea_, outArea_;
};

// ---------------------------------------------------------------- the bands over the spectrum
class CoreEqEditor::Graph final : public juce::Component, public juce::SettableTooltipClient {
public:
  explicit Graph(CoreEqEditor& e) : e_(e) {
    setTooltip(u8("Drag a point: frequency and gain. Wheel over a point: Q. Double-click: add a bell, or turn a band off. Right-click a point: more."));
  }
  juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced(36, 12).withTrimmedBottom(10); }
  float xOf(double f) const {
    const auto p = plot();
    return p.getX() + (float)(std::log2(std::clamp(f, kMinFreq, kMaxFreq) / kMinFreq) / std::log2(kMaxFreq / kMinFreq)) * p.getWidth();
  }
  double freqAt(float x) const {
    const auto p = plot();
    return std::clamp(kMinFreq * std::pow(kMaxFreq / kMinFreq, (double)((x - p.getX()) / p.getWidth())), kMinFreq, kMaxFreq);
  }
  float yOf(double db) const { const auto p = plot(); return p.getCentreY() - (float)(std::clamp(db, -kRange, kRange) / kRange) * p.getHeight() / 2; }
  double dbAt(float y) const { const auto p = plot(); return std::clamp((double)(p.getCentreY() - y) / (p.getHeight() / 2) * kRange, -kRange, kRange); }
  float yOfSpectrum(double db) const { const auto p = plot(); return p.getBottom() - (float)std::clamp((db + 84.0) / 84.0, 0.0, 1.0) * p.getHeight(); }

  juce::Point<float> node(int b) const {
    const BandView v = e_.band(b);
    return {xOf(v.freq), yOf(gained(v.shape) ? v.gain : 0.0)};
  }
  int hit(juce::Point<float> at) const {
    if (e_.band(e_.selected()).on && node(e_.selected()).getDistanceFrom(at) <= 10) return e_.selected();
    for (int b = kBands - 1; b >= 0; --b)
      if (e_.band(b).on && node(b).getDistanceFrom(at) <= 10) return b;
    return -1;
  }

  void paint(juce::Graphics& g) override {
    const auto p = plot();
    const auto all = getLocalBounds().toFloat();
    juce::ColourGradient bg(kField.darker(0.25f), all.getX(), all.getY(), kField.darker(0.05f), all.getX(), all.getBottom(), false);
    g.setGradientFill(bg);
    g.fillRoundedRectangle(all, theme::kCorner);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (double f : {30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
      const float x = xOf(f);
      g.setColour(theme::kGrid.withAlpha(0.8f));
      g.fillRect(x, p.getY(), 1.0f, p.getHeight());
      g.setColour(kDim);
      g.drawText(f >= 1000 ? juce::String((int)(f / 1000)) + "k" : juce::String((int)f), (int)x - 16, (int)p.getBottom() + 2, 32, 12,
                 juce::Justification::centred, false);
    }
    for (int db = -24; db <= 24; db += 6) {
      const float y = yOf(db);
      g.setColour(db == 0 ? kOutline : theme::kGrid.withAlpha(0.8f));
      g.fillRect(p.getX(), y, p.getWidth(), db == 0 ? 1.2f : 1.0f);
      g.setColour(kDim);
      g.drawText((db > 0 ? "+" : "") + juce::String(db), 0, (int)y - 6, 31, 12, juce::Justification::centredRight, false);
    }
    // The input (the window in front): 1/6-octave bands from 20 Hz, behind everything.
    const auto& bands = e_.spectrum();
    if (!bands.empty()) {
      juce::Path fill;
      fill.startNewSubPath(p.getX(), p.getBottom());
      for (size_t i = 0; i < bands.size(); ++i) {
        const double f = kMinFreq * std::pow(2.0, ((double)i + 0.5) / 6.0);
        if (f > kMaxFreq) break;
        fill.lineTo(xOf(f), yOfSpectrum(bands[i]));
      }
      fill.lineTo(p.getRight(), p.getBottom());
      fill.closeSubPath();
      g.setColour(theme::kPrimary.withAlpha(0.35f));
      g.fillPath(fill);
      g.setColour(theme::kPrimaryLight.withAlpha(0.45f));
      g.strokePath(fill, juce::PathStrokeType(1.0f));
    } else {
      g.setColour(kDim);
      g.drawText(u8("Play the song to see what comes into this EQ"), p.reduced(8).toNearestInt(), juce::Justification::topRight, false);
    }
    // Each band as a hill (or a valley) from the 0 dB line, in its blue; the chosen one on top.
    const int steps = std::max(2, (int)p.getWidth() / 3);
    std::vector<double> total((size_t)steps + 1, 0.0);
    std::vector<int> order;
    for (int b = 0; b < kBands; ++b) if (b != e_.selected()) order.push_back(b);
    order.push_back(e_.selected());
    for (int b : order) {
      const BandView v = e_.band(b);
      if (!v.on) continue;
      const bool chosen = b == e_.selected();
      juce::Path curve, area, dyn;
      area.startNewSubPath(p.getX(), yOf(0));
      for (int s = 0; s <= steps; ++s) {
        const float x = p.getX() + p.getWidth() * (float)s / (float)steps;
        const double f = freqAt(x), db = v.does() ? v.response(f, v.gain) : 0.0;
        total[(size_t)s] += db;
        const float y = yOf(db);
        if (s == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
        area.lineTo(x, y);
        if (v.dyn) {
          const float yd = yOf(v.response(f, v.gain + v.range));
          if (s == 0) dyn.startNewSubPath(x, yd); else dyn.lineTo(x, yd);
        }
      }
      area.lineTo(p.getRight(), yOf(0));
      area.closeSubPath();
      const juce::Colour c = bandColour(b), line = lineColour(b);
      juce::ColourGradient hill(c.withAlpha(chosen ? 0.42f : 0.26f), 0, yOf(gained(v.shape) && v.gain >= 0 ? kRange * 0.6 : -kRange * 0.6),
                                c.withAlpha(0.03f), 0, yOf(0), false);
      g.setGradientFill(hill);
      g.fillPath(area);
      g.setColour(line.withAlpha(chosen ? 1.0f : 0.7f));
      g.strokePath(curve, juce::PathStrokeType(chosen ? 1.8f : 1.2f));
      if (v.dyn) {
        juce::Path dashed;
        const float dashes[] = {4.0f, 4.0f};
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, dyn, dashes, 2);
        g.setColour(line.withAlpha(0.8f));
        g.fillPath(dashed);
      }
    }
    // The whole EQ.
    juce::Path sum;
    for (int s = 0; s <= steps; ++s) {
      const float x = p.getX() + p.getWidth() * (float)s / (float)steps, y = yOf(total[(size_t)s]);
      if (s == 0) sum.startNewSubPath(x, y); else sum.lineTo(x, y);
    }
    g.setColour(kText.withAlpha(0.9f));
    g.strokePath(sum, juce::PathStrokeType(2.0f));
    // The points: a ring in the band's blue with its number; the chosen one larger with a halo.
    for (int b : order) {
      const BandView v = e_.band(b);
      if (!v.on) continue;
      const bool chosen = b == e_.selected();
      const auto n = node(b);
      const float rad = chosen ? 8.5f : 7.0f;
      if (chosen) {
        g.setColour(lineColour(b).withAlpha(0.22f));
        g.fillEllipse(n.x - rad - 5, n.y - rad - 5, (rad + 5) * 2, (rad + 5) * 2);
      }
      juce::ColourGradient dot(bandColour(b).brighter(0.35f), n.x, n.y - rad, bandColour(b).darker(0.2f), n.x, n.y + rad, false);
      g.setGradientFill(dot);
      g.fillEllipse(n.x - rad, n.y - rad, rad * 2, rad * 2);
      g.setColour(chosen ? kText : juce::Colours::black.withAlpha(0.5f));
      g.drawEllipse(n.x - rad, n.y - rad, rad * 2, rad * 2, chosen ? 1.5f : 1.0f);
      g.setColour(inkOn(b));
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
      g.drawText(juce::String(b + 1), juce::Rectangle<float>(n.x - rad, n.y - rad, rad * 2, rad * 2), juce::Justification::centred, false);
    }
    g.setColour(kOutline);
    g.drawRoundedRectangle(all.reduced(0.5f), theme::kCorner, 1.0f);
  }

  void mouseDown(const juce::MouseEvent& ev) override {
    const int b = hit(ev.position);
    if (b < 0) return;
    if (b != e_.selected()) e_.select(b);
    if (ev.mods.isPopupMenu()) { showMenu(b); return; }
    drag_ = b;
    e_.beginGesture();
  }
  void mouseDrag(const juce::MouseEvent& ev) override {
    if (drag_ < 0) return;
    const BandView v = e_.band(drag_);
    std::vector<std::pair<std::string, double>> values = {{key(drag_, "Freq"), freqAt(ev.position.x)}};
    if (gained(v.shape)) values.push_back({key(drag_, "Gain"), std::round(dbAt(ev.position.y) * 10) / 10});
    e_.setMany(values, u8("EQ band"));
  }
  void mouseUp(const juce::MouseEvent&) override {
    if (drag_ < 0) return;
    drag_ = -1;
    e_.endGesture();
    repaint();
  }
  void mouseDoubleClick(const juce::MouseEvent& ev) override {
    const int b = hit(ev.position);
    if (b >= 0) { e_.set(key(b, "On"), 0, u8("Band off")); return; }
    e_.addBand(freqAt(ev.position.x), std::round(dbAt(ev.position.y) * 10) / 10);
  }
  void mouseWheelMove(const juce::MouseEvent& ev, const juce::MouseWheelDetails& w) override {
    const int b = hit(ev.position);
    if (b < 0 || w.deltaY == 0) return;
    const std::string k = key(b, "Q");
    const double next = std::clamp(e_.get(k) * (w.deltaY > 0 ? 1.12 : 1 / 1.12), 0.1, 18.0);
    e_.beginGesture();
    e_.set(k, next, u8("Band Q"));
    e_.endGesture();
  }

private:
  void showMenu(int b) {
    const BandView v = e_.band(b);
    juce::PopupMenu menu, shapes, slopes, chans;
    for (int s = 0; s < 6; ++s) shapes.addItem(100 + s, u8(shapeName(s)), true, v.shape == s);
    const char* slopeNames[] = {"12 dB/oct", "24 dB/oct", "48 dB/oct"};
    for (int s = 0; s < 3; ++s) slopes.addItem(200 + s, u8(slopeNames[s]), true, v.slope == s);
    const char* chanNames[] = {"Stereo", "Left", "Right", "Mid", "Side"};
    for (int c = 0; c < 5; ++c) chans.addItem(300 + c, u8(chanNames[c]), true, v.chan == c);
    menu.addSubMenu(u8("Shape"), shapes);
    menu.addSubMenu(u8("Slope"), slopes, isCut(v.shape));
    menu.addSubMenu(u8("Channel"), chans);
    menu.addItem(1, u8("Dynamic (moves with the level)"), gained(v.shape), v.dyn);
    menu.addSeparator();
    menu.addItem(3, u8("Duplicate"));
    menu.addItem(2, u8("Remove"));
    menu.addSeparator();
    menu.addItem(4, u8("Reset every band"));
    juce::Component::SafePointer<Graph> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [self, b, v](int id) {
      if (!self) return;
      CoreEqEditor& e = self->e_;
      if (id == 1) e.set(key(b, "Dyn"), v.dyn ? 0.0 : 1.0, u8("Dynamic band"));
      else if (id == 2) e.set(key(b, "On"), 0, u8("Band off"));
      else if (id == 3) e.duplicate();
      else if (id == 4) e.resetAll();
      else if (id >= 100 && id < 106) e.set(key(b, "Type"), id - 100, u8("Band shape"));
      else if (id >= 200 && id < 203) e.set(key(b, "Slope"), id - 200, u8("Band slope"));
      else if (id >= 300 && id < 305) e.set(key(b, "Chan"), id - 300, u8("Band channel"));
    });
  }

  CoreEqEditor& e_;
  int drag_ = -1;
};

// ---------------------------------------------------------------- one band
class CoreEqEditor::Card final : public juce::Component {
public:
  Card(CoreEqEditor& e, int b) : e_(e), b_(b), dot_(bandColour(b), u8("This band on or off")) {
    dot_.onClick = [this] { const bool on = e_.band(b_).on; e_.set(key(b_, "On"), on ? 0.0 : 1.0, on ? u8("Band off") : u8("Band on")); };
    const juce::Colour c = bandColour(b);
    // Buttons and lists in the card's own colour, never grey (Diogo, 2026-11-09).
    styleToggle(dyn_, u8("Dynamic"), u8("The gain moves by up to Range when this band is louder than Threshold"), c);
    // Dynamic in the band's own colour, a lighter tone of the header (Diogo, 2026-11-09).
    dyn_.setColour(juce::TextButton::buttonColourId, c.withAlpha(0.32f));
    dyn_.setColour(juce::TextButton::buttonOnColourId, c.withAlpha(0.7f));
    dyn_.setColour(juce::TextButton::textColourOffId, kText);
    dyn_.setColour(juce::TextButton::textColourOnId, kText);
    dyn_.onClick = [this] { e_.set(key(b_, "Dyn"), dyn_.getToggleState() ? 0.0 : 1.0, u8("Dynamic band")); };
    auto fill = [&](juce::ComboBox& box, const char* k, const juce::String& tip) {
      if (const doc::EffectParamInfo* p = e_.param(key(b_, k)))
        for (size_t i = 0; i < p->choices.size(); ++i) box.addItem(u8(p->choices[i]), (int)i + 1);
      box.setTooltip(tip);
      box.setWantsKeyboardFocus(false);
      box.setColour(juce::ComboBox::arrowColourId, lineColour(b_));
      const std::string name = key(b_, k);
      box.onChange = [this, &box, name] { if (box.getSelectedId() > 0) e_.set(name, box.getSelectedId() - 1, u8("Band")); };
      addAndMakeVisible(box);
    };
    fill(shape_, "Type", u8("The band's shape"));
    fill(chan_, "Chan", u8("Which part of the sound: both sides, left, right, the middle or the sides"));
    fill(slope_, "Slope", u8("How steep the cut is"));
    const char* names[3] = {"Freq", "Gain", "Q"};
    const char* labels[3] = {"Frequency", "Gain", "Q"};
    for (int k = 0; k < 3; ++k) {
      styleKnob(knobs_[k].knob, lineColour(b));
      e_.bind(knobs_[k].knob, key(b_, names[k]));
      if (k == 0)  // short enough to sit whole under the knob
        knobs_[k].knob.textFromValueFunction = [](double f) {
          return f >= 1000 ? juce::String(f / 1000, f >= 10000 ? 1 : 2) + " kHz" : juce::String((int)std::lround(f)) + " Hz";
        };
      knobs_[k].label.setText(u8(labels[k]), juce::dontSendNotification);
      knobs_[k].label.setFont(juce::FontOptions(theme::kFontBody));
      knobs_[k].knob.setColour(juce::Slider::textBoxOutlineColourId, kOutline);
      knobs_[k].knob.setColour(juce::Slider::textBoxBackgroundColourId, theme::kBackground.withAlpha(0.6f));
      knobs_[k].label.setJustificationType(juce::Justification::centred);
      knobs_[k].label.setColour(juce::Label::textColourId, kMuted);
      knobs_[k].label.setInterceptsMouseClicks(false, false);
      addAndMakeVisible(knobs_[k].label);
      addAndMakeVisible(knobs_[k].knob);
    }
    min_.setButtonText(u8("-"));
    min_.setTooltip(u8("Minimise: the band goes to the table at the side"));
    min_.setWantsKeyboardFocus(false);
    min_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    min_.setColour(juce::TextButton::textColourOffId, kText);
    min_.onClick = [this] { e_.fold(b_, true); };
    for (juce::Component* c2 : std::initializer_list<juce::Component*>{&dot_, &dyn_, &min_}) addAndMakeVisible(c2);
  }

  void refresh(const std::vector<std::string>& modulated) {
    const BandView v = e_.band(b_);
    on_ = v.on;
    dot_.set(v.on);
    dyn_.setToggleState(v.dyn, juce::dontSendNotification);
    dyn_.setVisible(gained(v.shape));  // only bells and shelves can move with the level
    shape_.setSelectedId(v.shape + 1, juce::dontSendNotification);
    chan_.setSelectedId(v.chan + 1, juce::dontSendNotification);
    slope_.setSelectedId(v.slope + 1, juce::dontSendNotification);
    slope_.setVisible(isCut(v.shape));
    knobs_[1].knob.setVisible(gained(v.shape));
    knobs_[1].label.setVisible(gained(v.shape));
    const char* names[3] = {"Freq", "Gain", "Q"};
    for (int k = 0; k < 3; ++k) {
      const bool lfo = std::find(modulated.begin(), modulated.end(), key(b_, names[k])) != modulated.end();
      knobs_[k].label.setColour(juce::Label::textColourId, lfo ? lineColour(b_) : kMuted);
      knobs_[k].label.setTooltip(lfo ? u8("Moved by an LFO") : juce::String());
    }
    resized();
    repaint();
  }

  void paint(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat();
    g.setColour(kCard.darker(0.1f));
    g.fillRoundedRectangle(r, theme::kCorner);
    // The header in the band's blue: a gradient with a light top edge, the body stays charcoal.
    const auto h = r.withHeight(34);
    const juce::Colour c = bandColour(b_);
    g.setColour(c.withAlpha(0.28f));
    juce::Path top;
    top.addRoundedRectangle(h.getX(), h.getY(), h.getWidth(), h.getHeight(), theme::kCorner, theme::kCorner, true, true, false, false);
    g.fillPath(top);
    g.setColour(c);
    g.fillRect(h.getX(), h.getY(), h.getWidth(), 2.0f);
    const BandView v = e_.band(b_);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));  // bigger (Diogo, 2026-11-09)
    // As the brief's card: On circle, "01  Low Mid" (the band and its region), the shape at the right in the band colour.
    auto line = h.toNearestInt().reduced(12, 0).withTrimmedLeft(28).withTrimmedRight(34);
    g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    g.setColour(c.interpolatedWith(juce::Colours::white, 0.3f));
    const juce::String shape = u8(shapeName(v.shape));
    const int sw = juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontStrong, juce::Font::bold)), shape) + 6;
    g.drawText(shape, line.removeFromRight(sw), juce::Justification::centredRight, false);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    g.drawText((b_ < 9 ? u8("0") : u8("")) + juce::String(b_ + 1) + u8("  ") + u8(regionName(v.freq)), line, juce::Justification::centredLeft, true);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    const int third = (titles_.getWidth() - 16) / 3;
    const juce::String last = isCut(v.shape) ? u8("Slope") : gained(v.shape) ? u8("Level") : juce::String();
    const juce::String names[3] = {u8("Shape"), u8("Channel"), last};
    for (int i = 0; i < 3; ++i)
      g.drawText(names[i], titles_.withX(titles_.getX() + i * (third + 8)).withWidth(third), juce::Justification::centredLeft, false);
  }
  void paintOverChildren(juce::Graphics& g) override {
    const auto r = getLocalBounds().toFloat();
    if (!on_) {  // a band that is off: the whole card darker and see-through
      g.setColour(theme::kBackground.withAlpha(0.6f));
      g.fillRoundedRectangle(r, theme::kCorner);
    }
    const bool chosen = e_.selected() == b_;
    // Every card has a border in its band colour (Diogo, 2026-11-09); the chosen one is thicker.
    g.setColour(lineColour(b_));
    g.drawRoundedRectangle(r.reduced(chosen ? 1.5f : 1.0f), theme::kCorner, chosen ? 3.0f : 2.0f);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(12, 0);
    auto head = r.removeFromTop(34);
    dot_.setBounds(head.removeFromLeft(20).withSizeKeepingCentre(20, 20));
    min_.setBounds(head.removeFromRight(24).withSizeKeepingCentre(24, 22));
    r.removeFromTop(6);
    // Shape, channel, then slope (cuts) or Dynamic (bells and shelves), each under its small title.
    titles_ = r.removeFromTop(16);
    auto row = r.removeFromTop(26);
    const int third = (row.getWidth() - 16) / 3;
    shape_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(8);
    chan_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(8);
    if (dyn_.isVisible()) dyn_.setBounds(row);
    if (slope_.isVisible()) slope_.setBounds(row);
    r.removeFromTop(10);
    r.removeFromBottom(10);
    // Always three columns, so every card has the same knob size; the shown knobs are centred.
    std::vector<int> shown;
    for (int k = 0; k < 3; ++k) if (knobs_[k].knob.isVisible()) shown.push_back(k);
    const int w = r.getWidth() / 3;
    const int x0 = r.getX() + (r.getWidth() - w * (int)shown.size()) / 2;
    for (size_t i = 0; i < shown.size(); ++i) {
      auto cell = juce::Rectangle<int>(x0 + (int)i * w, r.getY(), w, r.getHeight());
      knobs_[shown[i]].label.setBounds(cell.removeFromTop(16));
      knobs_[shown[i]].knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, std::min(76, w - 16), 24);
      knobs_[shown[i]].knob.setBounds(cell);
    }
  }
  void mouseDown(const juce::MouseEvent&) override { e_.select(b_); }

private:
  struct Knob { juce::Label label; juce::Slider knob; };
  CoreEqEditor& e_;
  int b_;
  bool on_ = false;
  juce::Rectangle<int> titles_;
  OnDot dot_;
  juce::TextButton dyn_, min_;
  juce::ComboBox shape_, chan_, slope_;
  std::array<Knob, 3> knobs_;
};

// ---------------------------------------------------------------- the editor
// ---------------------------------------------------------------- minimised bands, as a table
class CoreEqEditor::Table final : public juce::Component, public juce::SettableTooltipClient {
public:
  explicit Table(CoreEqEditor& e) : e_(e) {}
  std::vector<int> rows() const {
    std::vector<int> r;
    for (int b = 0; b < kBands; ++b) if (e_.folded(b)) r.push_back(b);
    return r;
  }
  void paint(juce::Graphics& g) override {
    const auto all = getLocalBounds();
    g.setColour(kCard.darker(0.1f));
    g.fillRoundedRectangle(all.toFloat(), theme::kCorner);
    auto r = all.reduced(10, 6);
    auto head = r.removeFromTop(kRow);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    const juce::String titles[kCols] = {u8(""), u8("Band"), u8("Shape"), u8("Freq"), u8("Gain"), u8("Q")};
    for (int c = 0; c < kCols; ++c) g.drawText(titles[c], cell(head, c), juce::Justification::centredLeft, false);
    g.setColour(kOutline);
    g.fillRect(r.getX(), r.getY(), r.getWidth(), 1);
    for (int b : rows()) {
      auto row = r.removeFromTop(kRow);
      const BandView v = e_.band(b);
      const juce::Colour c = bandColour(b);
      if (b == e_.selected()) { g.setColour(c.withAlpha(0.18f)); g.fillRect(row.expanded(4, 0)); }
      const auto dot = cell(row, 0).toFloat().withSizeKeepingCentre(12, 12);
      g.setColour(c.darker(0.75f));
      g.fillEllipse(dot);
      g.setColour(c);
      if (v.on) g.fillEllipse(dot.reduced(2));
      g.drawEllipse(dot, 1.2f);
      g.setColour(v.on ? kText : kDim);
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      g.drawText((b < 9 ? u8("0") : u8("")) + juce::String(b + 1), cell(row, 1), juce::Justification::centredLeft, false);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawText(u8(shapeName(v.shape)), cell(row, 2), juce::Justification::centredLeft, true);
      g.drawText(v.freq >= 1000 ? juce::String(v.freq / 1000, 2) + u8(" kHz") : juce::String((int)std::lround(v.freq)) + u8(" Hz"), cell(row, 3), juce::Justification::centredLeft, false);
      g.drawText(gained(v.shape) ? (v.gain > 0 ? u8("+") : u8("")) + juce::String(v.gain, 1) + u8(" dB") : u8("-"), cell(row, 4), juce::Justification::centredLeft, false);
      g.drawText(juce::String(v.q, 2), cell(row, 5), juce::Justification::centredLeft, false);
      g.setColour(kOutline.withAlpha(0.5f));
      g.fillRect(row.getX(), row.getBottom() - 1, row.getWidth(), 1);
    }
  }
  void mouseDown(const juce::MouseEvent& m) override {
    auto r = getLocalBounds().reduced(10, 6);
    r.removeFromTop(kRow);
    for (int b : rows()) {
      auto row = r.removeFromTop(kRow);
      if (!row.contains(m.getPosition())) continue;
      if (cell(row, 0).contains(m.getPosition())) {  // the circle: band on or off
        const bool on = e_.band(b).on;
        e_.set(key(b, "On"), on ? 0.0 : 1.0, on ? u8("Band off") : u8("Band on"));
      } else if (m.getNumberOfClicks() > 1) {
        e_.fold(b, false);  // a double click opens the card again
      } else {
        e_.select(b);
      }
      return;
    }
  }
  void mouseDoubleClick(const juce::MouseEvent& m) override { mouseDown(m); }
  int preferredHeight() const { return 12 + kRow * (1 + (int)rows().size()); }

private:
  static constexpr int kRow = 26, kCols = 6;
  static juce::Rectangle<int> cell(juce::Rectangle<int> row, int c) {
    static const float widths[kCols] = {0.08f, 0.12f, 0.24f, 0.22f, 0.20f, 0.14f};
    float x = 0;
    for (int i = 0; i < c; ++i) x += widths[i];
    return row.withX(row.getX() + (int)(x * row.getWidth())).withWidth((int)(widths[c] * row.getWidth()));
  }
  CoreEqEditor& e_;
};

CoreEqEditor::CoreEqEditor(CoreFxWindows& owner, std::string trackId, std::string fxId)
    : Pane(owner, std::move(trackId), std::move(fxId), "eq") {
  where_.setFont(juce::FontOptions(theme::kFontHeading, juce::Font::bold));
  where_.setColour(juce::Label::textColourId, kText);
  int id = 1;
  for (const auto& p : doc::effectPresets())
    if (std::string(p.effect) == "eq") { preset_.addItem(u8(p.name), id++); presets_.push_back(&p); }
  preset_.setTextWhenNothingSelected(presets_.empty() ? u8("Default") : u8("Presets"));
  preset_.setEnabled(!presets_.empty());
  preset_.setTooltip(u8("A whole EQ: every band"));
  preset_.onChange = [this] {
    const int i = preset_.getSelectedId() - 1;
    if (i < 0 || i >= (int)presets_.size()) return;
    const doc::EffectPreset* p = presets_[(size_t)i];
    edit([&](doc::Document& d, int at) { doc::applyEffectPreset(d, trackId_, at, *p); }, u8("Preset ") + u8(p->name));
  };
  ab_.setTooltip(u8("Compare two settings: A or B (your changes stay in the one shown)"));
  ab_.onChoose = [this](int s) { if (s != slot()) compare(s); };
  copy_.setButtonText(u8("Copy A → B"));
  copy_.setTooltip(u8("The lit one's bands into the other"));
  copy_.onClick = [this] { copyToOther(); };
  on_.onClick = [this] { const json* f = fx(); if (f) setBypass(!doc::effectBypassed(*f)); };

  kindLabel_.setText(u8("PARAMETRIC · DYNAMIC EQ"), juce::dontSendNotification);
  kindLabel_.setFont(juce::FontOptions(theme::kFontSmall));
  kindLabel_.setColour(juce::Label::textColourId, kMuted);
  styleToggle(autoGain_, u8("Auto Gain"), u8("Takes back the EQ's average boost or cut, so louder does not fool you"));
  autoGain_.onClick = [this] { set("autoGain", autoGain_.getToggleState() ? 0.0 : 1.0, u8("Auto gain")); };
  addBand_.setButtonText(u8("+"));
  addBand_.setTooltip(u8("Turn on the next band (a bell at 1 kHz)"));
  addBand_.onClick = [this] { addBand(1000, 0); };

  graph_ = std::make_unique<Graph>(*this);
  for (int b = 0; b < kBands; ++b) {
    auto& c = chips_[(size_t)b];
    c.setClickingTogglesState(false);
    c.setWantsKeyboardFocus(false);
    // Chips in the band's full colour with white text; the lightest bands take charcoal text (Diogo, 2026-11-09).
    const juce::Colour ink = bandColour(b).getPerceivedBrightness() > 0.75f ? theme::kBackground : juce::Colours::white;
    c.setColour(juce::TextButton::buttonColourId, bandColour(b).withAlpha(0.7f));
    c.setColour(juce::TextButton::buttonOnColourId, bandColour(b));
    c.setColour(juce::TextButton::textColourOffId, ink);
    c.setColour(juce::TextButton::textColourOnId, ink);
    c.onClick = [this, b] { select(b); };
    addChildComponent(c);
  }

  selName_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  selName_.setColour(juce::Label::textColourId, kText);
  const std::pair<juce::Label*, const char*> labels[] = {{&freqLabel_, "Frequency"}, {&qLabel_, "Q"}, {&gainLabel_, "Gain"},
                                                         {&thrLabel_, "Threshold"}, {&rangeLabel_, "Range"}, {&outLabel_, "Output"}};
  for (auto& [l, t] : labels) {
    l->setText(u8(t), juce::dontSendNotification);
    l->setFont(juce::FontOptions(theme::kFontBody));
    l->setColour(juce::Label::textColourId, kMuted);
    l->setJustificationType(juce::Justification::centredRight);
  }
  styleToggle(dyn_, u8("Dynamic"), u8("The gain moves by up to Range when this band is louder than Threshold"));
  dyn_.onClick = [this] { set(key(selected_, "Dyn"), dyn_.getToggleState() ? 0.0 : 1.0, u8("Dynamic band")); };
  bind(output_, "output");
  analyzer_.setText(u8("Analyzer: what comes in"), juce::dontSendNotification);
  analyzer_.setFont(juce::FontOptions(theme::kFontSmall));
  analyzer_.setColour(juce::Label::textColourId, kDim);

  for (juce::Component* c : std::initializer_list<juce::Component*>{
           &where_, &preset_, &ab_, &copy_, &on_, &kindLabel_, &autoGain_, &addBand_,
           graph_.get(), &selName_, &freqLabel_, &freq_, &qLabel_, &q_, &gainLabel_, &gain_, &dyn_, &thrLabel_, &thr_, &rangeLabel_, &range_,
           &outLabel_, &output_, &analyzer_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
  for (int b = 0; b < kBands; ++b) { cards_[(size_t)b] = std::make_unique<Card>(*this, b); addAndMakeVisible(*cards_[(size_t)b]); }
  table_ = std::make_unique<Table>(*this);
  table_->setTooltip(u8("Minimised bands. Click to choose, double-click to open the card, the circle turns the band on or off"));
  addChildComponent(*table_);
  // Bands that are off start minimised, so the open cards have room.
  for (int b = 0; b < kBands; ++b) folded_[(size_t)b] = !band(b).on;
  for (int n = 0; n < kLfos; ++n) {
    lfos_[(size_t)n] = std::make_unique<CoreFxWindows::Lfo>(*this, n, juce::Colour(n == 0 ? 0xffffffff : 0xffd6dbe7));  // LFOs in white tints, never a band's colour
    addAndMakeVisible(*lfos_[(size_t)n]);
  }
  // The first band that changes something is the chosen one.
  for (int b = 0; b < kBands; ++b) if (band(b).does()) { selected_ = b; break; }
  bindSelected();
  setSize(1320, 1000);
  refresh();
}

void CoreEqEditor::bindSelected() {
  for (juce::Slider* s : {&freq_, &q_, &gain_, &thr_, &range_}) unbind(*s);
  bind(freq_, key(selected_, "Freq"));
  bind(q_, key(selected_, "Q"));
  bind(gain_, key(selected_, "Gain"));
  bind(thr_, key(selected_, "Thr"));
  bind(range_, key(selected_, "Range"));
  const juce::Colour c = lineColour(selected_);
  for (juce::Slider* s : {&freq_, &q_, &gain_, &thr_, &range_}) s->setColour(juce::Slider::trackColourId, c.withAlpha(0.45f));
}

void CoreEqEditor::select(int b) {
  b = std::clamp(b, 0, kBands - 1);
  if (b == selected_) return;
  selected_ = b;
  bindSelected();
  refresh();
}

void CoreEqEditor::addBand(double freq, double gain) {
  for (int b = 0; b < kBands; ++b) {
    if (band(b).on) continue;
    setMany({{key(b, "On"), 1.0}, {key(b, "Type"), (double)Bell}, {key(b, "Freq"), std::clamp(freq, kMinFreq, kMaxFreq)},
             {key(b, "Gain"), gain}, {key(b, "Q"), 1.0}, {key(b, "Dyn"), 0.0}}, u8("Add band"));
    select(b);
    return;
  }
}

void CoreEqEditor::duplicate() {
  const BandView v = band(selected_);
  for (int b = 0; b < kBands; ++b) {
    if (band(b).on) continue;
    std::vector<std::pair<std::string, double>> values;
    for (const char* k : {"Type", "Gain", "Q", "Slope", "Chan", "Dyn", "Thr", "Range"}) values.push_back({key(b, k), get(key(selected_, k))});
    values.push_back({key(b, "Freq"), std::clamp(v.freq * 1.25, kMinFreq, kMaxFreq)});
    values.push_back({key(b, "On"), 1.0});
    setMany(values, u8("Duplicate band"));
    select(b);
    return;
  }
}

void CoreEqEditor::resetAll() {
  std::vector<std::pair<std::string, double>> values;
  if (info()) for (const auto& p : info()->params) values.push_back({p.name, p.def});
  setMany(values, u8("Reset EQ"));
}

void CoreEqEditor::spectrumChanged() { graph_->repaint(); }

void CoreEqEditor::refresh() {
  const json* f = fx();
  if (!f) return;
  juce::String where = u8("Master");
  if (const doc::Track* t = song().findTrack(trackId_)) where = u8(t->name);
  where_.setText(u8("CORE · EQ   ") + where, juce::dontSendNotification);
  // The title panel and its buttons carry the track's colour, never grey (Diogo, 2026-11-09).
  if (const doc::Track* t = song().findTrack(trackId_)) tint_ = trackColour(*t); else tint_ = kMasterColour;
  ab_.set(slot_, tint_);
  copy_.setColour(juce::TextButton::buttonColourId, tint_.withAlpha(0.35f));
  copy_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
  on_.tint(tint_);
  for (juce::TextButton* b : {&addBand_, &autoGain_}) {  // no grey buttons: the track's colour
    b->setColour(juce::TextButton::buttonColourId, tint_.withAlpha(0.35f));
    b->setColour(juce::TextButton::buttonOnColourId, tint_.withAlpha(0.8f));
    b->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    b->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
  }
  {  // the chosen band's Dynamic, in that band's colour
    const juce::Colour c = bandColour(selected_);
    dyn_.setColour(juce::TextButton::buttonColourId, c.withAlpha(0.32f));
    dyn_.setColour(juce::TextButton::buttonOnColourId, c.withAlpha(0.7f));
    dyn_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    dyn_.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
  }
  on_.set(!doc::effectBypassed(*f));
  autoGain_.setToggleState(get("autoGain") > 0.5, juce::dontSendNotification);
  copy_.setButtonText(slot_ == 0 ? u8("Copy A → B") : u8("Copy B → A"));
  refreshBound();
  const BandView sel = band(selected_);
  selName_.setText(bandTitle(selected_) + (sel.on ? juce::String() : u8("  (off)")), juce::dontSendNotification);
  gain_.setVisible(gained(sel.shape));
  gainLabel_.setVisible(gained(sel.shape));
  dyn_.setEnabled(gained(sel.shape));
  dyn_.setToggleState(sel.dyn, juce::dontSendNotification);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&thrLabel_, &thr_, &rangeLabel_, &range_}) c->setVisible(sel.dyn);
  int on = 0;
  for (int b = 0; b < kBands; ++b) {
    const BandView v = band(b);
    auto& c = chips_[(size_t)b];
    c.setVisible(v.on);
    c.setButtonText(juce::String(b + 1) + u8("  ") + u8(regionName(v.freq)) + u8(" · ") + u8(shapeName(v.shape)));
    c.setToggleState(b == selected_, juce::dontSendNotification);
    on += v.on;
  }
  addBand_.setEnabled(on < kBands);
  std::vector<std::string> modulated;
  for (auto& l : lfos_) l->refresh(modulated);
  for (int b = 0; b < kBands; ++b) {
    cards_[(size_t)b]->setVisible(!folded_[(size_t)b]);
    cards_[(size_t)b]->refresh(modulated);
  }
  table_->setVisible(!table_->rows().empty());
  table_->repaint();
  resized();
  graph_->repaint();
  repaint();
}

void CoreEqEditor::paint(juce::Graphics& g) {
  g.fillAll(kPanel);
  // The top bar: a soft gradient with a light edge.
  const auto t = topArea_.toFloat();
  juce::ColourGradient grad(kCard.brighter(0.08f), t.getX(), t.getY(), kPanel, t.getX(), t.getBottom(), false);
  g.setGradientFill(grad);
  g.fillRect(t);
  g.setColour(juce::Colours::black.withAlpha(0.4f));
  g.fillRect(t.getX(), t.getBottom() - 1, t.getWidth(), 1.0f);
  {  // the title panel: a see-through bar in the track's colour, a line under it and a border, like the LFOs
    const auto p = headArea_.toFloat();
    g.setColour(kCard);
    g.fillRoundedRectangle(p, theme::kCorner);
    const auto bar = p.withHeight(34);
    g.setColour(tint_.withAlpha(0.10f));
    g.fillRect(bar);
    g.setColour(tint_);
    g.fillRect(bar.getX(), bar.getBottom() - 1.0f, bar.getWidth(), 1.0f);
    g.drawRoundedRectangle(p.reduced(1.0f), theme::kCorner, 2.0f);
  }
  // Rule 11: the window's sections in a scale of the track's colour, light at the top (the title panel keeps the
  // track's own colour); the bands keep their own colours inside.
  theme::paintToneSection(g, bandsArea_.toFloat(), 30.0f, theme::toneScale(tint_, 3, 0), u8("EQ BANDS"));
  theme::paintToneSection(g, selArea_.toFloat(), 0.0f, theme::toneScale(tint_, 3, 1));
  theme::paintToneSection(g, outArea_.toFloat(), (float)outArea_.getHeight(), theme::toneScale(tint_, 3, 2), u8("OUTPUT"));
}

void CoreEqEditor::layoutCards(juce::Rectangle<int> r) {
  std::vector<Card*> shown;
  for (auto& c : cards_) if (c->isVisible()) shown.push_back(c.get());
  // Minimised bands sit in a table at the right (Diogo, 2026-11-09: "minimizarem para o lado, ficando uma tabela").
  if (table_->isVisible()) {
    auto t = r.removeFromRight(shown.empty() ? r.getWidth() : 380);
    table_->setBounds(t.withHeight(std::min(t.getHeight(), table_->preferredHeight())));
    r.removeFromRight(8);
  }
  if (shown.empty()) return;
  // Wide cards in up to two rows, as in the Core FX window (Diogo, 2026-11-09: "tudo tão apertado").
  const int gap = 8, cols = std::max(1, std::min({4, ((int)shown.size() + 1) / 2, r.getWidth() / 220}));
  const int rows = ((int)shown.size() + cols - 1) / cols;
  const int w = (r.getWidth() - gap * (cols - 1)) / cols;
  const int h = std::min(196, (r.getHeight() - gap * (rows - 1)) / std::max(1, rows));
  for (size_t i = 0; i < shown.size(); ++i)
    shown[i]->setBounds(r.getX() + (int)(i % cols) * (w + gap), r.getY() + (int)(i / cols) * (h + gap), w, h);
}

void CoreEqEditor::resized() {
  auto r = getLocalBounds();
  // Top bar: where, preset, A/B, copy and On on the left; the two LFOs take the rest (Diogo, 2026-11-09).
  topArea_ = r.removeFromTop(132);
  {
    auto top = topArea_.reduced(10, 8);
    // The title panel, like the LFOs: a bar with the title and the On circle, then the preset, A/B and Copy.
    headArea_ = top.removeFromLeft(300);
    auto left = headArea_.reduced(10, 0);
    auto bar = left.removeFromTop(34);
    on_.setBounds(bar.removeFromRight(20).withSizeKeepingCentre(20, 20));
    where_.setBounds(bar);
    left.removeFromTop(10);
    preset_.setBounds(left.removeFromTop(28).reduced(0, 1));
    left.removeFromTop(8);
    auto row = left.removeFromTop(28);
    ab_.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(8);
    copy_.setBounds(row.removeFromLeft(120).reduced(0, 1));
    top.removeFromLeft(12);
    const int lw = (top.getWidth() - 8) / 2;
    lfos_[0]->setBounds(top.removeFromLeft(lw));
    lfos_[1]->setBounds(top.removeFromRight(lw));
  }
  r.reduce(10, 6);
  outArea_ = r.removeFromBottom(40);
  {
    auto o = outArea_.reduced(10, 7).withTrimmedLeft(70);
    outLabel_.setBounds(o.removeFromLeft(54));
    output_.setBounds(o.removeFromLeft(150).reduced(2, 1));
    o.removeFromLeft(12);
    autoGain_.setBounds(o.removeFromLeft(90).reduced(0, 1));
    o.removeFromLeft(12);
    analyzer_.setBounds(o);
  }
  r.removeFromBottom(8);
  cardsArea_ = r.removeFromBottom(2 * 196 + 8);
  layoutCards(cardsArea_);
  r.removeFromBottom(8);
  selArea_ = r.removeFromBottom(40);
  {
    auto s = selArea_.reduced(10, 7);
    selName_.setBounds(s.removeFromLeft(220));
    for (auto [label, slider] : std::initializer_list<std::pair<juce::Label*, juce::Slider*>>{{&rangeLabel_, &range_}, {&thrLabel_, &thr_}}) {
      slider->setBounds(s.removeFromRight(110).reduced(2, 1));
      label->setBounds(s.removeFromRight(84));
    }
    dyn_.setBounds(s.removeFromRight(80).reduced(0, 1));
    s.removeFromRight(8);
    for (auto [label, slider] : std::initializer_list<std::pair<juce::Label*, juce::Slider*>>{{&gainLabel_, &gain_}, {&qLabel_, &q_}, {&freqLabel_, &freq_}}) {
      slider->setBounds(s.removeFromRight(110).reduced(2, 1));
      label->setBounds(s.removeFromRight(84));
    }
  }
  r.removeFromBottom(8);
  bandsArea_ = r;
  {
    auto b = bandsArea_.reduced(8, 0);
    auto head = b.removeFromTop(30).reduced(0, 3);
    head.removeFromLeft(90);
    addBand_.setBounds(head.removeFromRight(40).reduced(1, 0));
    head.removeFromRight(12);
    kindLabel_.setBounds(head.removeFromLeft(180));
    autoGain_.toFront(false);
    b.removeFromTop(8);
    auto chips = b.removeFromBottom(26);
    b.removeFromBottom(6);
    graph_->setBounds(b);
    for (auto& c : chips_) {
      if (!c.isVisible()) continue;
      const int w = juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontBody)), c.getButtonText()) + 28;
      c.setBounds(chips.removeFromLeft(w).reduced(0, 1));
      chips.removeFromLeft(6);
    }
    chips.removeFromBottom(6);
  }
}

CoreFxWindows::Pane* makeCoreEqEditor(CoreFxWindows& owner, const std::string& trackId, const std::string& fxId) {
  return new CoreEqEditor(owner, trackId, fxId);
}

}  // namespace core::app
