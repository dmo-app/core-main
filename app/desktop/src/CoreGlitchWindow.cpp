#include "CoreGlitchWindow.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Theme.h"
#include "TrackColour.h"
#include "Waveforms.h"
#include "edit.hpp"
#include "glitch.hpp"
#include "samples.hpp"

namespace core::app {

using json = nlohmann::json;

namespace {
juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }
juce::File fileOfUtf8(const std::string& path) { return juce::File(juce::String::fromUTF8(path.c_str())); }
using theme::kPanel, theme::kCard, theme::kField, theme::kText, theme::kMuted, theme::kDim, theme::kOutline, theme::kBackground;

// Each sampler's colour (his mockup, 2026-11-09): the accent, a light one, and eight slice colours (strong to light).
struct TabColours { juce::uint32 accent, light; juce::uint32 slices[8]; };
const TabColours kTabs[spath::kLayers] = {
    {0xff29b6f6, 0xffb3e5fc, {0xff0288d1, 0xff039be5, 0xff03a9f4, 0xff29b6f6, 0xff4fc3f7, 0xff81d4fa, 0xffb3e5fc, 0xff0277bd}},
    {0xffff8a2a, 0xffffc48a, {0xffffd2a6, 0xffff8a2a, 0xffe04e00, 0xff8a3a00, 0xffffb060, 0xffff6a10, 0xffb85a1a, 0xffffe6cc}},
    {0xff3cf08c, 0xffa8f5c8, {0xffb8f5d2, 0xff3cf08c, 0xff00a852, 0xff006b38, 0xff7af0b0, 0xff1ed070, 0xff2e9e66, 0xffdcfae8}},
    {0xfff05ce6, 0xfff5b8f0, {0xfff5c8f2, 0xfff05ce6, 0xffb000a8, 0xff6e0068, 0xfff590ee, 0xffd830cf, 0xff9a3a96, 0xfffbe0f8}}};

constexpr float kNodeR = 12.0f;          // the nodes: small circles, colour only (Diogo)
constexpr double kMapW = 1200, kMapH = 260;  // the map's own units (Node::x, y)
const char* kPatternNames[] = {"A", "B", "C", "D"};
const char* kFxLabels[] = {"Off", "Delay", "Crush", "Filter", "Stutter"};
const char* kEndLabels[] = {"Stop", "Back to start", "Repeat last"};
const double kFades[] = {0, 1, 3, 5, 10};

juce::String fmt(double v, const char* unit, int decimals = 0) {
  juce::String s = decimals > 0 ? juce::String(v, decimals) : juce::String((int)std::lround(v));
  if (unit && *unit) s << " " << unit;
  return s;
}

// A button drawn by the editor: dark or light, inverted on hover and when on (the UI rule of 2026-11-09).
void paintButton(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, bool over, juce::Colour accent,
                 float fontSize = theme::kFontSmall) {
  const bool lit = on != over;
  // Never a colourless button (Diogo 2026-11-09): off is a see-through tint of the sampler's colour with its edge.
  g.setColour(on ? accent : (over ? accent.withAlpha(0.6f) : accent.withAlpha(0.22f)));
  g.fillRoundedRectangle(r, 2.0f);
  g.setColour(on ? accent.brighter(0.2f) : accent.withAlpha(0.8f));
  g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 1.0f);
  g.setColour(lit ? juce::Colour(0xff07101a) : kText);
  g.setFont(juce::FontOptions(fontSize, juce::Font::bold));
  g.drawText(text, r, juce::Justification::centred, true);
}

// A card: a dark body, its title in capitals with the small square in the sampler's colour.
void paintGlitchCard(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, juce::Colour accent) {
  g.setColour(kCard);
  g.fillRoundedRectangle(r, 2.0f);
  g.setColour(kOutline);
  g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 1.0f);
  g.setColour(accent);
  g.fillRect(juce::Rectangle<float>(r.getX() + 10, r.getY() + 11, 7, 7));
  g.setColour(theme::kTitle);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  g.drawText(title.toUpperCase(), juce::Rectangle<float>(r.getX() + 24, r.getY() + 4, r.getWidth() - 30, 22), juce::Justification::centredLeft);
}
}  // namespace

// ---------------------------------------------------------------- the editor in one window
class CoreGlitchWindows::Editor final : public juce::Component, public juce::SettableTooltipClient {
public:
  Editor(CoreGlitchWindows& owner, std::string trackId) : owner_(owner), trackId_(std::move(trackId)), waves_([this] { repaint(); }) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setSize(1280, 880);
    documentChanged();
  }
  ~Editor() override { while (gestures_ > 0) endGesture(); }

  const std::string& trackId() const { return trackId_; }
  const doc::Document& song() const { return owner_.host_.currentDocument(); }
  const doc::Track* track() const {
    const doc::Track* t = song().findTrack(trackId_);
    return t && doc::isGlitch(*t) ? t : nullptr;
  }

  void documentChanged() {
    if (const doc::Track* t = track()) {
      try { patch_ = doc::glitchPatch(*t); } catch (const std::exception&) { patch_ = {}; }
    }
    const spath::Pattern& p = pattern();
    if (selected_ >= (int)p.nodes.size()) selected_ = p.nodes.empty() ? -1 : 0;
    if (selected_ < 0 && !p.nodes.empty()) selected_ = 0;
    repaint();
  }

  // 30 times a second while open: the ring that walks the path follows the song.
  void tick() {
    const double now = owner_.host_.playheadTick();
    if (now != lastPlayhead_) { lastPlayhead_ = now; repaint(mapArea_.toNearestInt().expanded(4)); repaint(barArea_.toNearestInt()); }
  }

  // ---- painting
  void paint(juce::Graphics& g) override {
    hits_.clear();
    g.fillAll(kBackground);
    if (const doc::Track* t = song().findTrack(trackId_)) {  // the track's colour around the window (ui-colours.md rule 1)
      g.setColour(trackColour(*t));
      g.fillRect(getLocalBounds().removeFromTop(3));
      g.drawRect(getLocalBounds(), 2);
    }
    const juce::Colour accent = accentColour();
    auto r = getLocalBounds().toFloat().reduced(12.0f);
    paintHeader(g, r.removeFromTop(40.0f), accent);
    r.removeFromTop(6.0f);
    paintTabs(g, r.removeFromTop(30.0f));
    r.removeFromTop(8.0f);
    barArea_ = r.removeFromBottom(40.0f);
    r.removeFromBottom(8.0f);
    auto low = r.removeFromBottom(176.0f);
    r.removeFromBottom(8.0f);
    auto top = r.removeFromTop(236.0f);
    r.removeFromTop(8.0f);
    const float w = top.getWidth();
    paintNodeCard(g, top.removeFromLeft(w * 0.33f), accent);
    top.removeFromLeft(8.0f);
    paintFxCard(g, top.removeFromRight(w * 0.25f), accent);
    top.removeFromRight(8.0f);
    paintSliceCard(g, top, accent);
    paintMapCard(g, r, accent);
    const float lw = low.getWidth();
    paintPathCard(g, low.removeFromLeft(lw * 0.58f), accent);
    low.removeFromLeft(8.0f);
    paintGlobalCard(g, low, accent);
    paintBar(g, barArea_, accent);
    if (!track()) {
      g.setColour(kBackground.withAlpha(0.85f));
      g.fillRect(getLocalBounds());
      g.setColour(kMuted);
      g.setFont(juce::FontOptions(theme::kFontStrong));
      g.drawText(u8("This track is no longer a Core Glitch."), getLocalBounds(), juce::Justification::centred);
    }
  }

  void resized() override { repaint(); }

  // ---- the mouse
  void mouseMove(const juce::MouseEvent& e) override {
    const int h = hitAt(e.position);
    if (h != hover_) { hover_ = h; repaint(); }
    setMouseCursor(h >= 0 && hits_[(size_t)h].kind == HitKind::Dial ? juce::MouseCursor::UpDownResizeCursor
                   : h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    setTooltip(h >= 0 ? hits_[(size_t)h].tip : juce::String());
  }
  void mouseExit(const juce::MouseEvent&) override { if (hover_ >= 0) { hover_ = -1; repaint(); } }

  void mouseDown(const juce::MouseEvent& e) override {
    dragging_ = false;
    linkFrom_ = -1;
    grabKeyboardFocus();
    if (!track()) return;
    const int h = hitAt(e.position);
    if (h < 0) {
      if (mapArea_.contains(e.position) && e.mods.isPopupMenu()) mapMenu(e.position);
      return;
    }
    const Hit hit = hits_[(size_t)h];
    if (hit.kind == HitKind::Dial) {
      if (e.mods.isAltDown()) { setDial(hit.id, dialDefault(hit.id), false); return; }
      drag_ = hit; dragging_ = true;
      dragStartY_ = e.position.y;
      dragStartValue_ = dialValue(hit.id);
      beginGesture(dialLabel(hit.id));
      return;
    }
    if (hit.kind == HitKind::Node) {
      selected_ = hit.index;
      if (e.mods.isPopupMenu()) { nodeMenu(hit.index); repaint(); return; }
      if (e.mods.isShiftDown()) { linkFrom_ = hit.index; linkTo_ = e.position; repaint(); return; }  // a new link
      drag_ = hit; dragging_ = true;
      beginGesture(u8("Core Glitch: move node"));
      repaint();
      return;
    }
    if (hit.kind == HitKind::SliceEdge) {
      drag_ = hit; dragging_ = true;
      beginGesture(u8("Core Glitch: slice"));
      return;
    }
    if (hit.action) hit.action(e);
  }

  void mouseDrag(const juce::MouseEvent& e) override {
    if (linkFrom_ >= 0) { linkTo_ = e.position; repaint(mapArea_.toNearestInt()); return; }
    if (!dragging_) return;
    const Hit hit = drag_;
    if (hit.kind == HitKind::Dial) {
      const DialSpec s = dialSpec(hit.id);
      const double range = s.max - s.min, fine = e.mods.isShiftDown() ? 0.1 : 1.0;
      double v = dragStartValue_ + (dragStartY_ - e.position.y) / 160.0 * range * fine;
      if (s.step > 0) v = std::round(v / s.step) * s.step;
      setDial(hit.id, std::clamp(v, s.min, s.max), true);
    } else if (hit.kind == HitKind::Node) {
      const auto at = fromMap(e.position);
      edit([&](spath::Patch& p) {
        auto& nodes = layerPattern(p).nodes;
        if (hit.index < (int)nodes.size()) { nodes[(size_t)hit.index].x = at.x; nodes[(size_t)hit.index].y = at.y; }
      }, u8("Core Glitch: move node"));
    } else if (hit.kind == HitKind::SliceEdge) {
      const double seconds = std::clamp(secondsAt(e.position.x), 0.0, soundSeconds());
      edit([&](spath::Patch& p) {
        auto& slices = p.layers[layer_].slices;
        if (hit.index >= (int)slices.size()) return;
        spath::Slice& s = slices[(size_t)hit.index];
        if (hit.id == 0) s.start = std::clamp(seconds, 0.0, s.end - 0.01);
        else s.end = std::max(seconds, s.start + 0.01);
      }, u8("Core Glitch: slice"));
    }
  }

  void mouseUp(const juce::MouseEvent& e) override {
    if (linkFrom_ >= 0) {
      const int h = hitAt(e.position);
      if (h >= 0 && hits_[(size_t)h].kind == HitKind::Node && hits_[(size_t)h].index != linkFrom_) addLink(linkFrom_, hits_[(size_t)h].index);
      linkFrom_ = -1;
      repaint();
    }
    if (dragging_) endGesture();
    dragging_ = false;
  }

  void mouseDoubleClick(const juce::MouseEvent& e) override {
    if (!track()) return;
    const int h = hitAt(e.position);
    if (h >= 0 && hits_[(size_t)h].kind == HitKind::Dial) { setDial(hits_[(size_t)h].id, dialDefault(hits_[(size_t)h].id), false); return; }
    if (h < 0 && mapArea_.contains(e.position)) addNode(fromMap(e.position));
  }

  bool keyPressed(const juce::KeyPress& key) override {
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && key.getKeyCode() == 'Z') { (mods.isShiftDown() ? owner_.cb_.redo : owner_.cb_.undo)(); return true; }
    if (mods.isCommandDown() && key.getKeyCode() == 'Y') { owner_.cb_.redo(); return true; }
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selected_ >= 0) { deleteNode(selected_); return true; }
    return false;
  }

private:
  enum class HitKind { Button, Dial, Node, SliceEdge };
  // The knobs: what each one changes. Values are read and written through dialValue / setDial.
  enum Dial {
    dStart, dEnd, dPitch, dFine, dLevel, dPan, dGate, dAttack, dRelease, dRepeats,
    dFxA, dFxB, dFxMix,  // + slot * 3
    dRate = 20, dVariation, dSeed, dBpm, dAmp, dGPitch, dSize, dDamp, dMix,
    dWeight = 40  // + exit index
  };
  struct DialSpec { const char* label; double min, max, def, step; const char* unit; int decimals; };
  struct Hit {
    juce::Rectangle<float> r;
    HitKind kind = HitKind::Button;
    int id = 0, index = 0;
    juce::String tip;
    std::function<void(const juce::MouseEvent&)> action;
  };

  int hitAt(juce::Point<float> p) const {
    for (int i = (int)hits_.size() - 1; i >= 0; --i)
      if (hits_[(size_t)i].r.contains(p)) return i;
    return -1;
  }
  void addButton(juce::Rectangle<float> r, std::function<void(const juce::MouseEvent&)> action, const juce::String& tip = {}) {
    Hit h;
    h.r = r; h.kind = HitKind::Button; h.action = std::move(action); h.tip = tip;
    hits_.push_back(std::move(h));
  }
  // A button that is drawn and clickable in one go.
  void button(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, std::function<void(const juce::MouseEvent&)> action,
              const juce::String& tip = {}, float fontSize = theme::kFontSmall) {
    addButton(r, std::move(action), tip);
    paintButton(g, r, text, on, isOver(r), accentColour(), fontSize);
  }
  bool isOver(juce::Rectangle<float> r) const { return isMouseOver() && r.contains(getMouseXYRelative().toFloat()); }

  // ---- the model
  juce::Colour accentColour() const { return juce::Colour(kTabs[layer_].accent); }
  juce::Colour sliceColour(int slice) const { return juce::Colour(kTabs[layer_].slices[(size_t)((slice % 8 + 8) % 8)]); }
  const spath::Layer& layer() const { return patch_.layers[layer_]; }
  const spath::Pattern& pattern() const { return patch_.layers[layer_].patterns[patch_.layers[layer_].pattern]; }
  spath::Pattern& layerPattern(spath::Patch& p) const { return p.layers[layer_].patterns[p.layers[layer_].pattern]; }
  const spath::Node* node() const { return selected_ >= 0 && selected_ < (int)pattern().nodes.size() ? &pattern().nodes[(size_t)selected_] : nullptr; }

  PatternEditor::Host& host() { return owner_.host_; }
  void beginGesture(const juce::String& label) { host().startGesture(label); ++gestures_; }
  void endGesture() { if (gestures_ > 0) { --gestures_; host().finishGesture(); } }

  // One change of the map: a copy, changed, written back (the document checks and clamps it).
  void edit(const std::function<void(spath::Patch&)>& change, const juce::String& label) {
    if (!track()) return;
    spath::Patch next = patch_;
    change(next);
    const std::string id = trackId_;
    try { host().applyEdit([&](doc::Document& d) { doc::setGlitchPatch(d, id, next); }, label); } catch (const std::exception&) {}
  }
  void setParam(const std::string& name, double value) {
    if (!track()) return;
    const std::string id = trackId_;
    try { host().applyEdit([&](doc::Document& d) { doc::setInstrumentParam(d, id, name, value); }, u8("Core Glitch: ") + u8(name)); }
    catch (const std::exception&) {}
  }
  double param(const char* name, double fallback) const {
    const doc::Track* t = track();
    const doc::InstrumentParamInfo* p = t ? doc::findInstrumentParam(*t, name) : nullptr;
    return p ? doc::instrumentParamValue(*t, *p) : fallback;
  }

  DialSpec dialSpec(int id) const {
    if (id >= dWeight) return {"Weight", 0, 100, 100, 1, "%", 0};
    switch (id) {
      case dStart: return {"Start", -0.5, 0.5, 0, 0.001, "s", 3};
      case dEnd: return {"End", -0.5, 0.5, 0, 0.001, "s", 3};
      case dPitch: return {"Pitch", -24, 24, 0, 1, "st", 0};
      case dFine: return {"Fine", -100, 100, 0, 1, "ct", 0};
      case dLevel: return {"Level", -30, 6, 0, 0.1, "dB", 1};
      case dPan: return {"Pan", -100, 100, 0, 1, "", 0};
      case dGate: return {"Gate", 5, 100, 100, 1, "%", 0};
      case dAttack: return {"Attack", 0, 50, 1, 0.1, "ms", 1};
      case dRelease: return {"Release", 1, 300, 40, 1, "ms", 0};
      case dRepeats: return {"Repeats", 1, 8, 1, 1, "x", 0};
      case dRate: return {"Rate", 0.5, 2, 1, 0.5, "x", 1};
      case dVariation: return {"Variation", 0, 100, 35, 1, "%", 0};
      case dSeed: return {"Seed", 0, 9999, 128, 1, "", 0};
      case dBpm: return {"BPM", 60, 300, 190, 1, "", 0};
      case dAmp: return {"Amp", -40, 6, -8, 0.1, "dB", 1};
      case dGPitch: return {"Pitch", -12, 12, 0, 1, "st", 0};
      case dSize: return {"Size", 0, 1, 0.45, 0.01, "", 2};
      case dDamp: return {"Damp", 0, 1, 0.58, 0.01, "", 2};
      case dMix: return {"Mix", 0, 1, 0.12, 0.01, "", 2};
      default: break;
    }
    const int k = (id - dFxA) % 3;
    return {k == 0 ? "A" : k == 1 ? "B" : "Mix", 0, 1, k == 2 ? 0 : 0.5, 0.01, "", 2};
  }
  double dialDefault(int id) const { return dialSpec(id).def; }
  juce::String dialLabel(int id) const { return u8("Core Glitch: ") + dialSpec(id).label; }
  // The FX knobs' names follow the type in the slot.
  juce::String fxKnobName(spath::FxType type, int k) const {
    static const char* names[5][3] = {{"A", "B", "Mix"}, {"Time", "Feedback", "Mix"}, {"Rate", "Bits", "Mix"},
                                      {"Cutoff", "Reso", "Mix"}, {"Grid", "Chance", "Mix"}};
    return u8(names[(int)type][k]);
  }

  double dialValue(int id) const {
    const spath::Pattern& p = pattern();
    if (id >= dWeight) {
      const spath::Node* n = node();
      const int i = id - dWeight;
      return n && i < (int)n->links.size() ? n->links[(size_t)i].weight : 0;
    }
    if (id >= dFxA && id < dFxA + 3 * spath::kFxSlots) {
      const spath::Fx& f = layer().fx[(id - dFxA) / 3];
      const int k = (id - dFxA) % 3;
      return k == 0 ? f.a : k == 1 ? f.b : f.mix;
    }
    switch (id) {
      case dRate: return p.rate;
      case dVariation: return p.variation;
      case dSeed: return p.seed;
      case dBpm: return patch_.bpm;
      case dAmp: return param("amp", -8);
      case dGPitch: return param("pitch", 0);
      case dSize: return param("size", 0.45);
      case dDamp: return param("damp", 0.58);
      case dMix: return param("mix", 0.12);
      default: break;
    }
    const spath::Node* n = node();
    if (!n) return dialDefault(id);
    switch (id) {
      case dStart: return n->startOffset;
      case dEnd: return n->endOffset;
      case dPitch: return n->pitch;
      case dFine: return n->fine;
      case dLevel: return n->level;
      case dPan: return n->pan;
      case dGate: return n->gate;
      case dAttack: return n->attackMs;
      case dRelease: return n->releaseMs;
      case dRepeats: return n->repeats;
      default: return 0;
    }
  }

  void setDial(int id, double v, bool inGesture) {
    if (std::fabs(v - dialValue(id)) < 1e-12) return;
    const juce::String label = dialLabel(id);
    if (id == dAmp) { setParam("amp", v); return; }
    if (id == dGPitch) { setParam("pitch", v); return; }
    if (id == dSize) { setParam("size", v); return; }
    if (id == dDamp) { setParam("damp", v); return; }
    if (id == dMix) { setParam("mix", v); return; }
    const int sel = selected_;
    edit([&](spath::Patch& p) {
      spath::Pattern& pat = layerPattern(p);
      if (id >= dWeight) {
        if (sel < 0 || sel >= (int)pat.nodes.size()) return;
        auto& links = pat.nodes[(size_t)sel].links;
        if (id - dWeight < (int)links.size()) links[(size_t)(id - dWeight)].weight = v;
        pat.shape.clear();
        return;
      }
      if (id >= dFxA && id < dFxA + 3 * spath::kFxSlots) {
        spath::Fx& f = p.layers[layer_].fx[(id - dFxA) / 3];
        const int k = (id - dFxA) % 3;
        (k == 0 ? f.a : k == 1 ? f.b : f.mix) = v;
        return;
      }
      switch (id) {
        case dRate: pat.rate = v; return;
        case dVariation: pat.variation = v; return;
        case dSeed: pat.seed = (uint32_t)std::lround(v); return;
        case dBpm: p.bpm = v; return;
        default: break;
      }
      if (sel < 0 || sel >= (int)pat.nodes.size()) return;
      spath::Node& n = pat.nodes[(size_t)sel];
      switch (id) {
        case dStart: n.startOffset = v; break;
        case dEnd: n.endOffset = v; break;
        case dPitch: n.pitch = v; break;
        case dFine: n.fine = v; break;
        case dLevel: n.level = v; break;
        case dPan: n.pan = v; break;
        case dGate: n.gate = v; break;
        case dAttack: n.attackMs = v; break;
        case dRelease: n.releaseMs = v; break;
        case dRepeats: n.repeats = (int)std::lround(v); break;
        default: break;
      }
    }, label);
    juce::ignoreUnused(inGesture);
  }

  // A knob: its name, the arc in the sampler's colour, the value in a box under it.
  void dial(juce::Graphics& g, juce::Rectangle<float> r, int id, const juce::String& name = {}) {
    const DialSpec s = dialSpec(id);
    const double v = dialValue(id);
    Hit h;
    h.r = r; h.kind = HitKind::Dial; h.id = id;
    h.tip = u8("Drag up or down (Shift: fine). Double-click or Alt+click: back to ") + fmt(s.def, s.unit, s.decimals) + ".";
    hits_.push_back(h);
    const juce::Colour accent = accentColour();
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(name.isEmpty() ? u8(s.label) : name, r.removeFromTop(14.0f), juce::Justification::centred);
    auto box = r.removeFromBottom(18.0f);
    const float size = std::min(r.getWidth(), r.getHeight()) - 4.0f;
    const auto c = juce::Rectangle<float>(size, size).withCentre(r.getCentre());
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float pos = (float)((v - s.min) / std::max(1e-9, s.max - s.min));
    juce::Path back, arc;
    back.addCentredArc(c.getCentreX(), c.getCentreY(), size / 2 - 2, size / 2 - 2, 0, a0, a1, true);
    g.setColour(kField.brighter(0.15f));
    g.strokePath(back, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const bool bipolar = s.min < 0 && s.max > 0;
    const float from = bipolar ? (float)((0 - s.min) / (s.max - s.min)) : 0.0f;
    const float lo = std::min(from, pos), hi = std::max(from, pos);
    if (hi > lo) {
      arc.addCentredArc(c.getCentreX(), c.getCentreY(), size / 2 - 2, size / 2 - 2, 0, a0 + (a1 - a0) * lo, a0 + (a1 - a0) * hi, true);
      g.setColour(accent);
      g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const float ang = a0 + (a1 - a0) * pos;
    g.setColour(kText);
    g.drawLine(c.getCentreX(), c.getCentreY(), c.getCentreX() + std::sin(ang) * (size / 2 - 6), c.getCentreY() - std::cos(ang) * (size / 2 - 6), 2.0f);
    box = box.withSizeKeepingCentre(std::min(box.getWidth(), 70.0f), box.getHeight());
    g.setColour(kField);
    g.fillRoundedRectangle(box, 2.0f);
    g.setColour(kOutline);
    g.drawRoundedRectangle(box.reduced(0.5f), 2.0f, 1.0f);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(fmt(v, s.unit, s.decimals), box, juce::Justification::centred);
  }

  // ---- the parts of the window
  void paintHeader(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    g.setColour(kPanel);
    g.fillRoundedRectangle(r, 2.0f);
    auto in = r.reduced(10.0f, 6.0f);
    g.setColour(accent);
    g.fillRect(juce::Rectangle<float>(in.getX(), in.getCentreY() - 4, 8, 8));
    in.removeFromLeft(16.0f);
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    g.drawText(u8("CORE GLITCH"), in.removeFromLeft(130.0f), juce::Justification::centredLeft);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    const doc::Track* t = track();
    g.drawText(u8("Sample Path") + (t ? u8(" \xc2\xb7 ") + u8(t->name) : juce::String()), in.removeFromLeft(320.0f), juce::Justification::centredLeft);
    auto right = in.removeFromRight(330.0f);
    button(g, right.removeFromRight(56.0f), u8("Redo"), false, [this](const juce::MouseEvent&) { owner_.cb_.redo(); }, u8("Redo (Ctrl+Y)"));
    right.removeFromRight(6.0f);
    button(g, right.removeFromRight(56.0f), u8("Undo"), false, [this](const juce::MouseEvent&) { owner_.cb_.undo(); }, u8("Undo (Ctrl+Z)"));
    right.removeFromRight(12.0f);
    button(g, right.removeFromRight(96.0f), u8("Host Sync"), patch_.sync,
           [this](const juce::MouseEvent&) { edit([](spath::Patch& p) { p.sync = !p.sync; }, u8("Core Glitch: Host Sync")); },
           u8("On: the steps follow the song's tempo. Off: the BPM in Path."));
    right.removeFromRight(10.0f);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(patch_.sync ? juce::String(song().bpm, 1) + u8(" BPM") : juce::String(patch_.bpm, 0) + u8(" BPM (own)"), right,
               juce::Justification::centredRight);
  }

  void paintTabs(juce::Graphics& g, juce::Rectangle<float> r) {
    const float w = std::min(150.0f, (r.getWidth() - 3 * 6) / spath::kLayers);
    for (int l = 0; l < spath::kLayers; ++l) {
      auto b = r.removeFromLeft(w);
      r.removeFromLeft(6.0f);
      const juce::Colour c(kTabs[l].accent);
      const bool on = l == layer_;
      const bool has = track() && doc::glitchSound(*track(), l) != nullptr;
      addButton(b, [this, l](const juce::MouseEvent&) { layer_ = l; selected_ = pattern().nodes.empty() ? -1 : 0; repaint(); },
                u8("Sampler ") + juce::String(l + 1) + (has ? juce::String() : u8(" (no sound yet)")));
      g.setColour(on ? c : (isOver(b) ? c.withAlpha(0.5f) : c.withAlpha(0.22f)));
      g.fillRoundedRectangle(b, 2.0f);
      g.setColour(c);
      g.fillRect(b.removeFromBottom(3.0f));
      g.setColour(on ? juce::Colour(0xff07101a) : kText);
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      g.drawText(u8("SAMPLER ") + juce::String(l + 1) + (patch_.layers[l].mute ? u8("  (muted)") : juce::String()), b, juce::Justification::centred);
      if (has && !on) { g.setColour(c); g.fillEllipse(b.getRight() - 14, b.getCentreY() - 3, 6, 6); }
    }
  }

  void paintNodeCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    const spath::Node* n = node();
    paintGlitchCard(g, r, n ? u8("Node ") + juce::String(n->id) : u8("Node"), accent);
    auto in = r.reduced(10.0f).withTrimmedTop(24.0f);
    if (!n) {
      g.setColour(kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawFittedText(u8("No node yet. Choose a sound in Sample \xc2\xb7 Slices, or double-click the map to add one."), in.toNearestInt(),
                       juce::Justification::centred, 3);
      return;
    }
    // Two rows of five knobs, then length, reverse and lock.
    const int rowsIds[2][5] = {{dStart, dEnd, dPitch, dFine, dRepeats}, {dLevel, dPan, dGate, dAttack, dRelease}};
    auto opts = in.removeFromBottom(26.0f);
    in.removeFromBottom(6.0f);
    const float rowH = in.getHeight() / 2;
    for (auto& row : rowsIds) {
      auto rr = in.removeFromTop(rowH);
      const float kw = rr.getWidth() / 5;
      for (int id : row) dial(g, rr.removeFromLeft(kw).reduced(2.0f), id);
    }
    const float ow = (opts.getWidth() - 12) / 3;
    const int sel = selected_;
    button(g, opts.removeFromLeft(ow), u8("Length ") + u8(n->length), false, [this, sel](const juce::MouseEvent&) { lengthMenu(sel); },
           u8("The node's step: 1/32 to 1/2, triplets too"));
    opts.removeFromLeft(6.0f);
    button(g, opts.removeFromLeft(ow), u8("Reverse"), n->reverse, [this, sel](const juce::MouseEvent&) {
      edit([&](spath::Patch& p) { auto& ns = layerPattern(p).nodes; if (sel < (int)ns.size()) ns[(size_t)sel].reverse = !ns[(size_t)sel].reverse; },
           u8("Core Glitch: reverse"));
    });
    opts.removeFromLeft(6.0f);
    button(g, opts, u8("Lock"), n->lock, [this, sel](const juce::MouseEvent&) {
      edit([&](spath::Patch& p) { auto& ns = layerPattern(p).nodes; if (sel < (int)ns.size()) ns[(size_t)sel].lock = !ns[(size_t)sel].lock; },
           u8("Core Glitch: lock node"));
    }, u8("Generate Variation leaves a locked node as it is"));
  }

  double soundSeconds() const {
    if (wave_ && !wave_->peaks.empty()) return (double)wave_->peaks.size() * wave_->framesPerPeak / wave_->sampleRate;
    double end = 0;
    for (const auto& s : layer().slices) end = std::max(end, s.end);
    return std::max(0.5, end);
  }
  double secondsAt(float x) const {
    return wavArea_.getWidth() > 0 ? (x - wavArea_.getX()) / wavArea_.getWidth() * soundSeconds() : 0.0;
  }
  float xAt(double seconds) const { return wavArea_.getX() + (float)(seconds / std::max(1e-6, soundSeconds())) * wavArea_.getWidth(); }

  void paintSliceCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    const doc::Track* t = track();
    const doc::SampleRef* sound = t ? doc::glitchSound(*t, layer_) : nullptr;
    paintGlitchCard(g, r, u8("Sample \xc2\xb7 Slices"), accent);
    auto head = juce::Rectangle<float>(r.getRight() - 300, r.getY() + 5, 290, 20);
    button(g, head.removeFromRight(70.0f), u8("Slice"), false, [this](const juce::MouseEvent& e) { sliceMenu(e); },
           u8("Cut the sound again: even parts or at its hits (the nodes keep their slice numbers)"), 11.5f);
    head.removeFromRight(6.0f);
    button(g, head.removeFromRight(120.0f), sound ? u8(sound->name.empty() ? std::string("Sound") : sound->name) : u8("Choose sound"), false,
           [this](const juce::MouseEvent&) { chooseSound(); }, u8("The sound of this sampler"), 11.5f);
    auto in = r.reduced(10.0f).withTrimmedTop(24.0f);
    // The whole sound on top, the slices as tiles under it.
    wavArea_ = in.removeFromTop(in.getHeight() * 0.52f);
    in.removeFromTop(6.0f);
    g.setColour(kField);
    g.fillRoundedRectangle(wavArea_, 2.0f);
    wave_.reset();
    if (sound) wave_ = waves_.get(sound->file, fileOfUtf8(sound->file));
    const auto& slices = layer().slices;
    for (size_t i = 0; i < slices.size(); ++i) {
      const float x0 = xAt(slices[i].start), x1 = xAt(slices[i].end);
      g.setColour(sliceColour((int)i).withAlpha(0.18f));
      g.fillRect(juce::Rectangle<float>(x0, wavArea_.getY(), std::max(1.0f, x1 - x0), wavArea_.getHeight()));
    }
    if (wave_) {
      const int cols = (int)wavArea_.getWidth();
      for (int c = 0; c < cols; ++c) {
        const double a = secondsAt(wavArea_.getX() + (float)c), b = secondsAt(wavArea_.getX() + (float)c + 1);
        const float pk = wave_->peakBetween(a, b);
        int slice = -1;
        for (size_t i = 0; i < slices.size(); ++i) if (a >= slices[i].start && a < slices[i].end) slice = (int)i;
        g.setColour(slice >= 0 ? sliceColour(slice) : kDim);
        const float hh = pk * wavArea_.getHeight() * 0.48f;
        g.fillRect(juce::Rectangle<float>(wavArea_.getX() + (float)c, wavArea_.getCentreY() - hh, 1.0f, std::max(1.0f, 2 * hh)));
      }
    } else {
      g.setColour(kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(sound ? u8("Reading the sound\xe2\x80\xa6") : u8("No sound yet: Choose sound"), wavArea_, juce::Justification::centred);
    }
    // The slice edges: drag to move them.
    for (size_t i = 0; i < slices.size(); ++i)
      for (int edge = 0; edge < 2; ++edge) {
        const float x = xAt(edge == 0 ? slices[i].start : slices[i].end);
        g.setColour(sliceColour((int)i));
        g.fillRect(juce::Rectangle<float>(x - (edge == 0 ? 0.0f : 1.0f), wavArea_.getY(), 1.5f, wavArea_.getHeight()));
        Hit h;
        h.r = juce::Rectangle<float>(x - 4, wavArea_.getY(), 8, wavArea_.getHeight());
        h.kind = HitKind::SliceEdge; h.index = (int)i; h.id = edge;
        h.tip = u8("Slice ") + juce::String((int)i + 1) + (edge == 0 ? u8(": start") : u8(": end")) + u8(" (drag)");
        hits_.push_back(h);
      }
    // The tiles: one per slice, its own part of the waveform; a click gives the chosen node that slice.
    if (slices.empty()) return;
    const float tw = std::min(90.0f, (in.getWidth() - 4.0f * (float)(slices.size() - 1)) / (float)slices.size());
    const spath::Node* n = node();
    for (size_t i = 0; i < slices.size(); ++i) {
      auto tile = juce::Rectangle<float>(in.getX() + (float)i * (tw + 4), in.getY(), tw, in.getHeight());
      const juce::Colour c = sliceColour((int)i);
      const bool chosen = n && n->slice == (int)i;
      g.setColour(kField);
      g.fillRoundedRectangle(tile, 2.0f);
      if (wave_) {
        const int cols = std::max(1, (int)tile.getWidth() - 4);
        for (int col = 0; col < cols; ++col) {
          const double a = slices[i].start + (slices[i].end - slices[i].start) * col / cols;
          const double b = slices[i].start + (slices[i].end - slices[i].start) * (col + 1) / cols;
          const float hh = wave_->peakBetween(a, b) * (tile.getHeight() - 18) * 0.48f;
          g.setColour(c);
          g.fillRect(juce::Rectangle<float>(tile.getX() + 2 + (float)col, tile.getCentreY() + 6 - hh, 1.0f, std::max(1.0f, 2 * hh)));
        }
      }
      g.setColour(c);
      g.drawRoundedRectangle(tile.reduced(0.5f), 2.0f, chosen ? 2.5f : 1.0f);
      g.setColour(kText);
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
      g.drawText(juce::String((int)i + 1), tile.withHeight(16.0f), juce::Justification::centred);
      const int sel = selected_;
      addButton(tile, [this, sel, i](const juce::MouseEvent&) {
        edit([&](spath::Patch& p) { auto& ns = layerPattern(p).nodes; if (sel >= 0 && sel < (int)ns.size()) ns[(size_t)sel].slice = (int)i; },
             u8("Core Glitch: node slice"));
      }, u8("Slice ") + juce::String((int)i + 1) + u8(": click to give it to the chosen node"));
    }
  }

  void paintFxCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    paintGlitchCard(g, r, u8("Node FX \xc2\xb7 Sampler ") + juce::String(layer_ + 1), accent);
    auto in = r.reduced(10.0f).withTrimmedTop(24.0f);
    const float slotH = in.getHeight() / spath::kFxSlots;
    for (int s = 0; s < spath::kFxSlots; ++s) {
      auto slot = in.removeFromTop(slotH).reduced(0, 3);
      const spath::Fx& f = layer().fx[s];
      auto head = slot.removeFromTop(22.0f);
      button(g, head.removeFromLeft(110.0f), u8(kFxLabels[(int)f.type]), f.type != spath::FxType::Off,
             [this, s](const juce::MouseEvent&) { fxMenu(s); }, u8("The effect in this slot"));
      const float kw = slot.getWidth() / 3;
      for (int k = 0; k < 3; ++k) dial(g, slot.removeFromLeft(kw).reduced(2.0f), dFxA + s * 3 + k, fxKnobName(f.type, k));
    }
  }

  // ---- the map
  juce::Point<float> toMap(double x, double y) const {
    const float sx = mapArea_.getWidth() / (float)kMapW, sy = mapArea_.getHeight() / (float)kMapH;
    return {mapArea_.getX() + (float)x * sx, mapArea_.getY() + (float)y * sy};
  }
  juce::Point<double> fromMap(juce::Point<float> p) const {
    const double sx = kMapW / std::max(1.0f, mapArea_.getWidth()), sy = kMapH / std::max(1.0f, mapArea_.getHeight());
    return {std::clamp((p.x - mapArea_.getX()) * sx, 20.0, kMapW - 20.0), std::clamp((p.y - mapArea_.getY()) * sy, 20.0, kMapH - 20.0)};
  }

  // Where the walk is at the playhead: the latest note of this track that holds there, walked from its start (the
  // engine's walk, the same steps; the song's first tempo).
  struct Now { int node = -1; double progress = 0; };
  Now walkNow() const {
    Now out;
    const doc::Track* t = track();
    const spath::Pattern& p = pattern();
    if (!t || p.nodes.empty()) return out;
    const double tick = owner_.host_.playheadTick();
    double since = -1;  // ticks since the note-on
    for (const auto& c : t->clips) {
      if (tick < (double)c.startTick || tick >= (double)(c.startTick + c.lengthTicks)) continue;
      const doc::Pattern* pat = nullptr;
      for (const auto& x : t->patterns) if (x.id == c.patternId) pat = &x;
      if (!pat || pat->lengthTicks <= 0) continue;
      const double local = std::fmod(tick - (double)c.startTick + (double)c.offsetTicks, (double)pat->lengthTicks);
      for (const auto& n : pat->notes)
        if (local >= (double)n.tick && local < (double)(n.tick + n.length) && (since < 0 || local - (double)n.tick < since)) since = local - (double)n.tick;
    }
    if (since < 0) return out;
    const double bpm = patch_.sync ? song().bpm : patch_.bpm;
    const double beats = since / doc::kPpq;
    spath::Walk w;
    w.reset(p);
    double at = 0;
    for (int guard = 0; guard < 100000 && w.node >= 0; ++guard) {
      const double len = spath::stepBeats(p, p.nodes[(size_t)w.node]);
      if (beats < at + len) { out.node = w.node; out.progress = (beats - at) / len; return out; }
      at += len;
      w.advance(p);
    }
    juce::ignoreUnused(bpm);
    return out;
  }

  void paintMapCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    paintGlitchCard(g, r, u8("Sample Path \xc2\xb7 Pattern ") + kPatternNames[layer().pattern], accent);
    // The legend: primary and alternate exits.
    auto legend = juce::Rectangle<float>(r.getRight() - 330, r.getY() + 6, 320, 18);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.setColour(kText);
    g.drawLine(legend.getX(), legend.getCentreY(), legend.getX() + 22, legend.getCentreY(), 2.0f);
    g.drawText(u8("Primary path"), legend.withTrimmedLeft(28).withWidth(100), juce::Justification::centredLeft);
    const float dash[] = {5.0f, 4.0f};
    g.drawDashedLine(juce::Line<float>(legend.getX() + 140, legend.getCentreY(), legend.getX() + 162, legend.getCentreY()), dash, 2, 1.5f);
    g.drawText(u8("Alternate exit"), legend.withTrimmedLeft(168).withWidth(110), juce::Justification::centredLeft);
    mapArea_ = r.reduced(12.0f).withTrimmedTop(22.0f);
    g.setColour(kField);
    g.fillRoundedRectangle(mapArea_, 2.0f);
    g.setColour(theme::kGrid);
    for (int i = 1; i < 12; ++i) g.drawVerticalLine((int)(mapArea_.getX() + mapArea_.getWidth() * i / 12), mapArea_.getY(), mapArea_.getBottom());
    for (int i = 1; i < 4; ++i) g.drawHorizontalLine((int)(mapArea_.getY() + mapArea_.getHeight() * i / 4), mapArea_.getX(), mapArea_.getRight());
    const spath::Pattern& p = pattern();
    if (p.nodes.empty()) {
      g.setColour(kDim);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawText(u8("Double-click to add a node. Shift+drag from one node to another to link them."), mapArea_, juce::Justification::centred);
      return;
    }
    // The links: in the colour of the node they leave; the main exit solid, the others dashed; in Weighted, the chance.
    for (size_t i = 0; i < p.nodes.size(); ++i) {
      const spath::Node& a = p.nodes[i];
      double total = 0;
      for (const auto& l : a.links) total += l.weight;
      for (const auto& l : a.links) {
        if (l.to < 0 || l.to >= (int)p.nodes.size()) continue;
        const spath::Node& b = p.nodes[(size_t)l.to];
        auto pa = toMap(a.x, a.y), pb = toMap(b.x, b.y);
        const juce::Point<float> d = pb - pa;
        const float len = std::max(1.0f, d.getDistanceFromOrigin());
        const juce::Point<float> u(d.x / len, d.y / len);
        // A slight bend, so a link and its way back do not overlap.
        const juce::Point<float> nrm(-u.y, u.x);
        const auto from = pa + u * kNodeR, to = pb - u * (kNodeR + 2.0f);
        const auto mid = (from + to) * 0.5f + nrm * 14.0f;
        juce::Path path;
        path.startNewSubPath(from);
        path.quadraticTo(mid, to);
        const juce::Colour c = sliceColour(a.slice);
        g.setColour(c.withAlpha(p.mode == spath::Mode::Follow && !l.main ? 0.45f : 0.95f));
        if (l.main) g.strokePath(path, juce::PathStrokeType(2.2f));
        else {
          juce::Path dashed;
          juce::PathStrokeType(1.6f).createDashedStroke(dashed, path, dash, 2);
          g.fillPath(dashed);
        }
        // The arrow head.
        const juce::Point<float> dir = (to - mid) / std::max(1.0f, (to - mid).getDistanceFromOrigin());
        juce::Path head;
        head.addTriangle(to, to - dir * 9.0f + juce::Point<float>(-dir.y, dir.x) * 4.5f, to - dir * 9.0f - juce::Point<float>(-dir.y, dir.x) * 4.5f);
        g.fillPath(head);
        if (p.mode == spath::Mode::Weighted && a.links.size() > 1 && total > 0) {
          g.setColour(kText);
          g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
          g.drawText(juce::String((int)std::lround(100 * l.weight / total)) + "%",
                     juce::Rectangle<float>(40, 14).withCentre(mid + nrm * 6.0f), juce::Justification::centred);
        }
      }
    }
    // A link being drawn.
    if (linkFrom_ >= 0 && linkFrom_ < (int)p.nodes.size()) {
      g.setColour(accent);
      const auto a = toMap(p.nodes[(size_t)linkFrom_].x, p.nodes[(size_t)linkFrom_].y);
      g.drawDashedLine(juce::Line<float>(a, linkTo_), dash, 2, 1.5f);
    }
    // The nodes: a small circle in the colour of its slice, the white number; the chosen one ringed, the start marked.
    const Now now = walkNow();
    for (size_t i = 0; i < p.nodes.size(); ++i) {
      const spath::Node& n = p.nodes[i];
      const auto c = toMap(n.x, n.y);
      const auto circle = juce::Rectangle<float>(2 * kNodeR, 2 * kNodeR).withCentre(c);
      Hit h;
      h.r = circle.expanded(3.0f); h.kind = HitKind::Node; h.index = (int)i;
      h.tip = u8("Node ") + juce::String(n.id) + u8(": click to choose, drag to move, Shift+drag to link, right-click for more");
      hits_.push_back(h);
      if ((int)i == p.start) {
        g.setColour(kText.withAlpha(0.7f));
        g.drawEllipse(circle.expanded(5.0f), 1.0f);
      }
      g.setColour(sliceColour(n.slice));
      g.fillEllipse(circle);
      if ((int)i == selected_) {
        g.setColour(kText);
        g.drawEllipse(circle.expanded(2.5f), 2.0f);
      }
      if (n.lock) {
        g.setColour(kText);
        g.fillRect(juce::Rectangle<float>(4, 4).withCentre(c + juce::Point<float>(kNodeR, -kNodeR)));
      }
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      // A thin dark edge keeps the white number readable on the light colours.
      g.setColour(juce::Colour(0x8c0b1018));
      for (auto off : {juce::Point<float>(-1, 0), juce::Point<float>(1, 0), juce::Point<float>(0, -1), juce::Point<float>(0, 1)})
        g.drawText(juce::String(n.id), circle + off, juce::Justification::centred);
      g.setColour(juce::Colours::white);
      g.drawText(juce::String(n.id), circle, juce::Justification::centred);
      if ((int)i == now.node) {  // the ring that walks: it fills round the node during its step
        juce::Path ring;
        ring.addCentredArc(c.x, c.y, kNodeR + 6, kNodeR + 6, 0, 0, juce::MathConstants<float>::twoPi * (float)std::clamp(now.progress, 0.02, 1.0), true);
        g.setColour(accent.brighter(0.4f));
        g.strokePath(ring, juce::PathStrokeType(2.5f));
      }
    }
  }

  void paintPathCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    paintGlitchCard(g, r, u8("Path"), accent);
    const spath::Pattern& p = pattern();
    auto in = r.reduced(10.0f).withTrimmedTop(24.0f);
    auto knobs = in.removeFromLeft(300.0f);
    const float kw = knobs.getWidth() / 4;
    dial(g, knobs.removeFromLeft(kw).reduced(2.0f), dRate);
    dial(g, knobs.removeFromLeft(kw).reduced(2.0f), dVariation);
    dial(g, knobs.removeFromLeft(kw).reduced(2.0f), dSeed);
    if (!patch_.sync) dial(g, knobs.reduced(2.0f), dBpm);
    else {
      g.setColour(kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawFittedText(u8("BPM\nfrom the song"), knobs.toNearestInt(), juce::Justification::centred, 2);
    }
    in.removeFromLeft(10.0f);
    // Mode, end, shapes.
    auto col = in.removeFromLeft(220.0f);
    auto row = col.removeFromTop(24.0f);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(u8("Mode"), row.removeFromLeft(44.0f), juce::Justification::centredLeft);
    button(g, row.removeFromLeft(84.0f), u8("Follow"), p.mode == spath::Mode::Follow,
           [this](const juce::MouseEvent&) { edit([&](spath::Patch& q) { layerPattern(q).mode = spath::Mode::Follow; }, u8("Core Glitch: Follow")); },
           u8("Every node takes its main exit"));
    row.removeFromLeft(4.0f);
    button(g, row.removeFromLeft(84.0f), u8("Weighted"), p.mode == spath::Mode::Weighted,
           [this](const juce::MouseEvent&) { edit([&](spath::Patch& q) { layerPattern(q).mode = spath::Mode::Weighted; }, u8("Core Glitch: Weighted")); },
           u8("Every node picks an exit by its weights, from the seed (the same every time)"));
    col.removeFromTop(6.0f);
    row = col.removeFromTop(24.0f);
    g.setColour(kMuted);
    g.drawText(u8("At end"), row.removeFromLeft(44.0f), juce::Justification::centredLeft);
    button(g, row, u8(kEndLabels[(int)p.end]), false, [this](const juce::MouseEvent&) { endMenu(); },
           u8("What a node with no exit does: stop, go back to the start node, or repeat"));
    col.removeFromTop(6.0f);
    g.setColour(kMuted);
    g.drawText(u8("Shapes"), col.removeFromTop(16.0f), juce::Justification::centredLeft);
    auto shapes = col.removeFromTop(48.0f);
    const auto& names = spath::shapes();
    for (size_t i = 0; i < names.size(); ++i) {
      auto cell = juce::Rectangle<float>(shapes.getX() + (float)(i % 2) * (shapes.getWidth() / 2), shapes.getY() + (float)(i / 2) * 24.0f,
                                         shapes.getWidth() / 2 - 4, 22.0f);
      const std::string shape = names[i];
      button(g, cell, u8(shape), p.shape == shape, [this, shape](const juce::MouseEvent&) {
        edit([&](spath::Patch& q) { spath::applyShape(layerPattern(q), shape); }, u8("Core Glitch: ") + u8(shape));
      }, p.lockPath ? u8("Lock Path is on: shapes leave the links alone") : u8("New links in this shape"));
    }
    in.removeFromLeft(12.0f);
    // The chosen node's exits: target, weight, main; Lock Path.
    auto exits = in;
    auto lockRow = exits.removeFromBottom(24.0f);
    button(g, lockRow.removeFromLeft(110.0f), u8("Lock Path"), p.lockPath,
           [this](const juce::MouseEvent&) { edit([&](spath::Patch& q) { layerPattern(q).lockPath = !layerPattern(q).lockPath; }, u8("Core Glitch: Lock Path")); },
           u8("Keeps the links as they are: Generate Variation and the shapes leave them alone"));
    g.setColour(kMuted);
    g.drawText(u8("Exits"), exits.removeFromTop(16.0f), juce::Justification::centredLeft);
    const spath::Node* n = node();
    if (!n || n->links.empty()) {
      g.setColour(kDim);
      g.drawFittedText(n ? u8("No exit: the end behaviour plays here. Shift+drag to another node.") : juce::String(), exits.toNearestInt(),
                       juce::Justification::centredLeft, 3);
      return;
    }
    double total = 0;
    for (const auto& l : n->links) total += l.weight;
    const int sel = selected_;
    for (size_t i = 0; i < n->links.size() && exits.getHeight() >= 22; ++i) {
      const spath::Link& l = n->links[i];
      auto line = exits.removeFromTop(24.0f);
      const spath::Node& to = pattern().nodes[(size_t)l.to];
      g.setColour(sliceColour(to.slice));
      g.fillEllipse(juce::Rectangle<float>(10, 10).withCentre({line.getX() + 6, line.getCentreY()}));
      g.setColour(kText);
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
      g.drawText(u8("\xe2\x86\x92 ") + juce::String(to.id), line.removeFromLeft(48.0f).withTrimmedLeft(14), juce::Justification::centredLeft);
      // The weight: drag it; shown with its chance.
      auto wbox = line.removeFromLeft(110.0f).reduced(0, 2);
      Hit h;
      h.r = wbox; h.kind = HitKind::Dial; h.id = dWeight + (int)i;
      h.tip = u8("Weight: drag up or down. The chance is this weight against the others.");
      hits_.push_back(h);
      g.setColour(kField);
      g.fillRoundedRectangle(wbox, 2.0f);
      g.setColour(accent.withAlpha(0.8f));
      g.fillRoundedRectangle(wbox.withWidth(wbox.getWidth() * (float)(l.weight / 100.0)), 2.0f);
      g.setColour(kText);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String((int)std::lround(l.weight)) + u8(" \xc2\xb7 ") + juce::String(total > 0 ? (int)std::lround(100 * l.weight / total) : 0) + "%",
                 wbox, juce::Justification::centred);
      line.removeFromLeft(6.0f);
      button(g, line.removeFromLeft(56.0f).reduced(0, 2), u8("Main"), l.main, [this, sel, i](const juce::MouseEvent&) {
        edit([&](spath::Patch& q) {
          auto& ns = layerPattern(q).nodes;
          if (sel < 0 || sel >= (int)ns.size()) return;
          for (size_t k = 0; k < ns[(size_t)sel].links.size(); ++k) ns[(size_t)sel].links[k].main = k == i;
          layerPattern(q).shape.clear();
        }, u8("Core Glitch: main exit"));
      }, u8("The exit Follow takes"), 11.0f);
      line.removeFromLeft(4.0f);
      button(g, line.removeFromLeft(24.0f).reduced(0, 2), u8("\xc3\x97"), false, [this, sel, i](const juce::MouseEvent&) {
        edit([&](spath::Patch& q) {
          auto& ns = layerPattern(q).nodes;
          if (sel < 0 || sel >= (int)ns.size() || i >= ns[(size_t)sel].links.size()) return;
          ns[(size_t)sel].links.erase(ns[(size_t)sel].links.begin() + (long)i);
          layerPattern(q).shape.clear();
        }, u8("Core Glitch: remove exit"));
      }, u8("Remove this exit"), 12.0f);
    }
  }

  void paintGlobalCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    paintGlitchCard(g, r, u8("Global"), accent);
    auto in = r.reduced(10.0f).withTrimmedTop(24.0f);
    auto toggles = in.removeFromRight(96.0f);
    const float kw = in.getWidth() / 5;
    for (int id : {dAmp, dGPitch, dSize, dDamp, dMix}) dial(g, in.removeFromLeft(kw).reduced(2.0f), id);
    const float th = (toggles.getHeight() - 12) / 3;
    button(g, toggles.removeFromTop(th), u8("Sync"), patch_.sync,
           [this](const juce::MouseEvent&) { edit([](spath::Patch& p) { p.sync = !p.sync; }, u8("Core Glitch: Sync")); }, u8("The steps follow the song's tempo"));
    toggles.removeFromTop(6.0f);
    const bool limiter = param("limiter", 1) >= 0.5;
    button(g, toggles.removeFromTop(th), u8("Limiter"), limiter, [this, limiter](const juce::MouseEvent&) { setParam("limiter", limiter ? 0 : 1); },
           u8("A limiter at the end of Core Glitch"));
    toggles.removeFromTop(6.0f);
    const int l = layer_;
    button(g, toggles, u8("Mute"), layer().mute,
           [this, l](const juce::MouseEvent&) { edit([l](spath::Patch& p) { p.layers[l].mute = !p.layers[l].mute; }, u8("Core Glitch: mute sampler")); },
           u8("Mutes this sampler (its path walks on, in time)"));
  }

  void paintBar(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent) {
    g.setColour(kPanel);
    g.fillRoundedRectangle(r, 2.0f);
    auto in = r.reduced(6.0f);
    const spath::Pattern& p = pattern();
    const Now now = walkNow();
    // Node buttons, square.
    auto nodes = in.removeFromLeft(std::min(in.getWidth() * 0.45f, 30.0f * (float)std::max<size_t>(1, p.nodes.size())));
    for (size_t i = 0; i < p.nodes.size() && nodes.getWidth() >= 26; ++i) {
      auto b = nodes.removeFromLeft(28.0f).withSizeKeepingCentre(26.0f, 26.0f);
      const juce::Colour c = sliceColour(p.nodes[i].slice);
      const bool on = (int)i == selected_;
      addButton(b, [this, i](const juce::MouseEvent&) { selected_ = (int)i; repaint(); }, u8("Node ") + juce::String(p.nodes[i].id));
      g.setColour(on ? c : c.withAlpha(0.22f));
      g.fillRoundedRectangle(b, 2.0f);
      g.setColour((int)i == now.node ? kText : c);
      g.drawRoundedRectangle(b.reduced(0.5f), 2.0f, (int)i == now.node ? 2.0f : 1.2f);
      g.setColour(on ? juce::Colour(0xff07101a) : kText);
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
      g.drawText(juce::String(p.nodes[i].id), b, juce::Justification::centred);
    }
    in.removeFromLeft(14.0f);
    // Patterns A..D: an empty one starts as a copy of the current.
    for (int k = 0; k < spath::kPatterns; ++k) {
      auto b = in.removeFromLeft(28.0f).withSizeKeepingCentre(26.0f, 26.0f);
      const int l = layer_;
      button(g, b, u8(kPatternNames[k]), layer().pattern == k, [this, l, k](const juce::MouseEvent&) {
        edit([&](spath::Patch& q) {
          spath::Layer& L = q.layers[l];
          if (!L.used[k]) { L.patterns[k] = L.patterns[L.pattern]; L.used[k] = true; }
          L.pattern = k;
        }, u8("Core Glitch: pattern ") + kPatternNames[k]);
        selected_ = 0;
      }, layer().used[k] ? u8("Pattern ") + kPatternNames[k] : u8("Pattern ") + kPatternNames[k] + u8(" (empty: starts as a copy of this one)"));
    }
    in.removeFromLeft(14.0f);
    auto right = in.removeFromRight(330.0f);
    button(g, right.removeFromLeft(160.0f), u8("Generate Variation"), false, [this](const juce::MouseEvent&) {
      edit([&](spath::Patch& q) {
        spath::generateVariation(layerPattern(q));  // from the seed and the pattern: the next press gives the next variation
      }, u8("Core Glitch: Generate Variation"));
    }, u8("Changes repeats, pitch, reverse and (without Lock Path) the links of the unlocked nodes, within Variation %"));
    right.removeFromLeft(6.0f);
    button(g, right.removeFromLeft(80.0f), u8("Start here"), node() && pattern().start == selected_, [this](const juce::MouseEvent&) {
      const int sel = selected_;
      edit([&](spath::Patch& q) { if (sel >= 0) layerPattern(q).start = sel; }, u8("Core Glitch: start node"));
    }, u8("The walk starts at the chosen node"));
    right.removeFromLeft(6.0f);
    const double fade = p.fadeMs;
    button(g, right, fade <= 0 ? u8("Hard cut") : u8("Fade ") + juce::String((int)std::lround(fade)) + u8(" ms"), false,
           [this](const juce::MouseEvent&) { fadeMenu(); }, u8("The shortest fade in and out of every part (Hard cut: none)"));
    juce::ignoreUnused(accent);
  }

  // ---- menus and actions
  void lengthMenu(int sel) {
    juce::PopupMenu menu;
    int id = 1;
    for (const auto& l : spath::kLengths) menu.addItem(id++, u8(l.name), true, node() && node()->length == l.name);
    juce::Component::SafePointer<Editor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, sel](int chosen) {
      if (!safe || chosen < 1) return;
      const std::string name = spath::kLengths[chosen - 1].name;
      safe->edit([&](spath::Patch& p) { auto& ns = safe->layerPattern(p).nodes; if (sel >= 0 && sel < (int)ns.size()) ns[(size_t)sel].length = name; },
                 u8("Core Glitch: length ") + u8(name));
    });
  }
  void fxMenu(int slot) {
    juce::PopupMenu menu;
    for (int t = 0; t < 5; ++t) menu.addItem(t + 1, u8(kFxLabels[t]), true, (int)layer().fx[slot].type == t);
    juce::Component::SafePointer<Editor> safe(this);
    const int l = layer_;
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, slot, l](int chosen) {
      if (!safe || chosen < 1) return;
      safe->edit([&](spath::Patch& p) {
        spath::Fx& f = p.layers[l].fx[slot];
        f.type = (spath::FxType)(chosen - 1);
        if (f.type != spath::FxType::Off && f.mix <= 0) f.mix = 0.3;  // heard at once
      }, u8("Core Glitch: ") + kFxLabels[chosen - 1]);
    });
  }
  void endMenu() {
    juce::PopupMenu menu;
    for (int k = 0; k < 3; ++k) menu.addItem(k + 1, u8(kEndLabels[k]), true, (int)pattern().end == k);
    juce::Component::SafePointer<Editor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe](int chosen) {
      if (!safe || chosen < 1) return;
      safe->edit([&](spath::Patch& p) { safe->layerPattern(p).end = (spath::End)(chosen - 1); }, u8("Core Glitch: at end"));
    });
  }
  void fadeMenu() {
    juce::PopupMenu menu;
    for (int k = 0; k < 5; ++k) menu.addItem(k + 1, kFades[k] <= 0 ? u8("Hard cut") : juce::String((int)kFades[k]) + u8(" ms"), true, pattern().fadeMs == kFades[k]);
    juce::Component::SafePointer<Editor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe](int chosen) {
      if (!safe || chosen < 1) return;
      safe->edit([&](spath::Patch& p) { safe->layerPattern(p).fadeMs = kFades[chosen - 1]; }, u8("Core Glitch: fade"));
    });
  }
  void nodeMenu(int index) {
    juce::PopupMenu menu;
    menu.addItem(1, u8("Start here"));
    menu.addItem(2, u8("Remove its exits"));
    menu.addItem(3, u8("Delete node"));
    juce::Component::SafePointer<Editor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, index](int chosen) {
      if (!safe || chosen < 1) return;
      if (chosen == 1) safe->edit([&](spath::Patch& p) { safe->layerPattern(p).start = index; }, u8("Core Glitch: start node"));
      if (chosen == 2)
        safe->edit([&](spath::Patch& p) {
          auto& ns = safe->layerPattern(p).nodes;
          if (index < (int)ns.size()) ns[(size_t)index].links.clear();
          safe->layerPattern(p).shape.clear();
        }, u8("Core Glitch: remove exits"));
      if (chosen == 3) safe->deleteNode(index);
    });
  }
  void mapMenu(juce::Point<float> at) {
    juce::PopupMenu menu;
    menu.addItem(1, u8("Add node here"));
    juce::Component::SafePointer<Editor> safe(this);
    const auto where = fromMap(at);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, where](int chosen) {
      if (safe && chosen == 1) safe->addNode(where);
    });
  }

  void addNode(juce::Point<double> at) {
    const spath::Pattern& p = pattern();
    if ((int)p.nodes.size() >= spath::kMaxNodes) return;
    const int slice = node() ? node()->slice : 0;
    const int index = (int)p.nodes.size();
    edit([&](spath::Patch& q) {
      spath::Pattern& pat = layerPattern(q);
      spath::Node n;
      n.id = std::max(1, pat.nextId);
      pat.nextId = n.id + 1;
      n.slice = std::min(slice, std::max(0, (int)q.layers[layer_].slices.size() - 1));
      n.x = at.x; n.y = at.y;
      pat.nodes.push_back(n);
    }, u8("Core Glitch: add node"));
    selected_ = index;
  }
  void deleteNode(int index) {
    edit([&](spath::Patch& q) {
      spath::Pattern& pat = layerPattern(q);
      if (index < 0 || index >= (int)pat.nodes.size()) return;
      pat.nodes.erase(pat.nodes.begin() + index);
      // Links after it move one place down; links to it go.
      for (auto& n : pat.nodes) {
        n.links.erase(std::remove_if(n.links.begin(), n.links.end(), [&](const spath::Link& l) { return l.to == index; }), n.links.end());
        for (auto& l : n.links) if (l.to > index) --l.to;
      }
      if (pat.start == index) pat.start = 0;
      else if (pat.start > index) --pat.start;
      pat.shape.clear();
    }, u8("Core Glitch: delete node"));
    selected_ = std::min(selected_, (int)pattern().nodes.size() - 1);
  }
  void addLink(int from, int to) {
    edit([&](spath::Patch& q) {
      spath::Pattern& pat = layerPattern(q);
      if (from >= (int)pat.nodes.size() || to >= (int)pat.nodes.size()) return;
      auto& links = pat.nodes[(size_t)from].links;
      for (const auto& l : links) if (l.to == to) return;
      if ((int)links.size() >= spath::kMaxLinks) return;
      links.push_back({to, links.empty() ? 100.0 : 50.0, links.empty()});
      pat.shape.clear();
    }, u8("Core Glitch: link"));
  }

  // The sound of this sampler: from the library (by role, as W-Core's Grain) or a WAV file.
  void chooseSound() {
    const doc::SampleIndex* index = owner_.cb_.sampleLibrary ? owner_.cb_.sampleLibrary() : nullptr;
    juce::PopupMenu menu;
    menu.addItem(1, u8("From a file\xe2\x80\xa6"));
    if (track() && doc::glitchSound(*track(), layer_)) menu.addItem(2, u8("No sound"));
    menu.addSeparator();
    std::map<std::string, juce::PopupMenu> byRole;
    std::map<std::string, int> counts;
    if (index)
      for (size_t i = 0; i < index->samples.size(); ++i) {
        const doc::LibrarySample& s = index->samples[i];
        const std::string role = s.info.role.empty() ? std::string("other") : s.info.role;
        if (++counts[role] > 80) continue;  // a menu stays readable
        byRole[role].addItem((int)i + 10, u8(s.displayName.empty() ? s.name : s.displayName));
      }
    if (byRole.empty()) menu.addItem(juce::PopupMenu::Item(u8("The sample library has not been read yet (Library)")).setEnabled(false));
    for (auto& [role, sub] : byRole) menu.addSubMenu(u8(role), sub);
    juce::Component::SafePointer<Editor> safe(this);
    const int l = layer_;
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe, index, l](int chosen) {
      if (!safe || chosen < 1) return;
      if (chosen == 1) { safe->chooseFile(l); return; }
      if (chosen == 2) { safe->setSound(l, nullptr, 0); return; }
      if (!index || chosen - 10 >= (int)index->samples.size()) return;
      const doc::LibrarySample& s = index->samples[(size_t)chosen - 10];
      const doc::SampleRef zone = doc::sampleRef(s);
      safe->setSound(l, &zone, s.seconds > 0 ? s.seconds : readSeconds(fileOfUtf8(s.file)));
    });
  }
  void chooseFile(int l) {
    chooser_ = std::make_unique<juce::FileChooser>(u8("A sound for Sampler ") + juce::String(l + 1),
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav");
    juce::Component::SafePointer<Editor> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, l](const juce::FileChooser& fc) {
      if (!safe) return;
      const juce::File f = fc.getResult();
      if (!f.existsAsFile()) return;
      doc::SampleRef zone;
      zone.file = f.getFullPathName().toStdString();
      zone.name = f.getFileNameWithoutExtension().toStdString();
      safe->setSound(l, &zone, readSeconds(f));
    });
  }
  static double readSeconds(const juce::File& file) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    return reader && reader->sampleRate > 0 ? (double)reader->lengthInSamples / reader->sampleRate : 0.0;
  }
  void setSound(int l, const doc::SampleRef* zone, double seconds) {
    if (!track()) return;
    const std::string id = trackId_;
    try {
      host().applyEdit([&](doc::Document& d) { doc::setGlitchSound(d, id, l, zone, seconds); },
                       u8("Core Glitch: sound of Sampler ") + juce::String(l + 1));
    } catch (const std::exception&) {}
    selected_ = pattern().nodes.empty() ? -1 : 0;
  }
  void sliceMenu(const juce::MouseEvent&) {
    juce::PopupMenu even, hits;
    for (int n : {2, 4, 6, 8, 12, 16, 24, 32}) even.addItem(n, juce::String(n) + u8(" parts"));
    for (int n : {4, 6, 8, 12, 16, 32}) hits.addItem(100 + n, u8("Up to ") + juce::String(n));
    juce::PopupMenu menu;
    menu.addSubMenu(u8("Even"), even);
    menu.addSubMenu(u8("At its hits"), hits);
    juce::Component::SafePointer<Editor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [safe](int chosen) {
      if (!safe || chosen < 1) return;
      safe->reslice(chosen >= 100 ? chosen - 100 : chosen, chosen >= 100);
    });
  }
  // New slices; the nodes keep their slice numbers (clamped to the new count).
  void reslice(int n, bool atHits) {
    const doc::Track* t = track();
    const doc::SampleRef* sound = t ? doc::glitchSound(*t, layer_) : nullptr;
    if (!sound) return;
    std::vector<spath::Slice> slices;
    if (atHits) {
      juce::AudioFormatManager formats;
      formats.registerBasicFormats();
      std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(fileOfUtf8(sound->file)));
      if (!reader || reader->sampleRate <= 0) return;
      const int frames = (int)std::min<juce::int64>(reader->lengthInSamples, (juce::int64)(reader->sampleRate * 120));
      juce::AudioBuffer<float> buf((int)std::max(1u, reader->numChannels), frames);
      if (!reader->read(&buf, 0, frames, 0, true, true)) return;
      slices = spath::transientSlices(buf.getReadPointer(0), buf.getReadPointer(std::min(1, buf.getNumChannels() - 1)), frames, reader->sampleRate, n);
    } else {
      const double seconds = readSeconds(fileOfUtf8(sound->file));
      if (seconds <= 0) return;
      slices = spath::evenSlices(seconds, n);
    }
    if (slices.empty()) return;
    const int l = layer_;
    edit([&](spath::Patch& p) {
      p.layers[l].slices = slices;
      for (auto& pat : p.layers[l].patterns)
        for (auto& node : pat.nodes) node.slice = std::min(node.slice, (int)slices.size() - 1);
    }, atHits ? u8("Core Glitch: slices at the hits") : u8("Core Glitch: even slices"));
  }

  CoreGlitchWindows& owner_;
  std::string trackId_;
  spath::Patch patch_;
  int layer_ = 0, selected_ = -1;
  std::vector<Hit> hits_;
  int hover_ = -1, gestures_ = 0;
  Hit drag_;
  bool dragging_ = false;
  float dragStartY_ = 0;
  double dragStartValue_ = 0, lastPlayhead_ = -1;
  int linkFrom_ = -1;
  juce::Point<float> linkTo_;
  juce::Rectangle<float> mapArea_, wavArea_, barArea_;
  WaveformCache waves_;
  std::shared_ptr<const Waveform> wave_;
  std::unique_ptr<juce::FileChooser> chooser_;
};

// ---------------------------------------------------------------- one window
class CoreGlitchWindows::Window final : public juce::DocumentWindow {
public:
  Window(CoreGlitchWindows& owner, const std::string& trackId)
      : juce::DocumentWindow(u8("Core Glitch"), kBackground, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton), owner_(owner) {
    editor_ = new Editor(owner, trackId);
    setUsingNativeTitleBar(true);
    setContentOwned(editor_, true);
    setResizable(true, false);
    setResizeLimits(1100, 760, 2600, 1800);
    retitle();
  }
  Editor& editor() { return *editor_; }
  void retitle() {
    const doc::Track* t = editor_->song().findTrack(editor_->trackId());
    setName(u8("Core Glitch \xc2\xb7 ") + (t ? u8(t->name) : u8("?")));
  }
  void closeButtonPressed() override {
    setVisible(false);
    juce::Component::SafePointer<Window> self(this);
    juce::MessageManager::callAsync([self] { if (self) self->owner_.close(self.getComponent()); });
  }

private:
  CoreGlitchWindows& owner_;
  Editor* editor_ = nullptr;
};

// ---------------------------------------------------------------- the windows
CoreGlitchWindows::CoreGlitchWindows(PatternEditor::Host& host, Callbacks callbacks) : host_(host), cb_(std::move(callbacks)) {}

CoreGlitchWindows::~CoreGlitchWindows() {
  stopTimer();
  windows_.clear();
}

void CoreGlitchWindows::open(const std::string& trackId) {
  const doc::Track* t = host_.currentDocument().findTrack(trackId);
  if (!t || !doc::isGlitch(*t)) return;
  for (auto& w : windows_)
    if (w->editor().trackId() == trackId) {
      w->setVisible(true);
      w->toFront(true);
      return;
    }
  auto w = std::make_unique<Window>(*this, trackId);
  // Each new window a little lower and to the right of the last, so several can be seen at once.
  const int step = 28 * (int)(windows_.size() % 8);
  w->centreWithSize(w->getWidth(), w->getHeight());
  w->setTopLeftPosition(w->getX() + step, w->getY() + step);
  w->setVisible(true);
  windows_.push_back(std::move(w));
  windows_.back()->toFront(true);
  if (!isTimerRunning()) startTimerHz(30);
}

void CoreGlitchWindows::documentChanged() {
  std::vector<Window*> gone;
  for (auto& w : windows_) {
    const doc::Track* t = host_.currentDocument().findTrack(w->editor().trackId());
    if (!t || !doc::isGlitch(*t)) { gone.push_back(w.get()); continue; }
    w->editor().documentChanged();
    w->retitle();
  }
  for (Window* w : gone) close(w);
}

void CoreGlitchWindows::closeAll() {
  windows_.clear();
  stopTimer();
}

void CoreGlitchWindows::timerCallback() {
  for (auto& w : windows_)
    if (w->isVisible()) w->editor().tick();
}

void CoreGlitchWindows::close(Window* w) {
  windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [w](const std::unique_ptr<Window>& x) { return x.get() == w; }), windows_.end());
  if (windows_.empty()) stopTimer();
}

} // namespace core::app
