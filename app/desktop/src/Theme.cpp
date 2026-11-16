#include "Theme.h"
#include "Language.h"
#include <cmath>

namespace core::app::theme {

namespace {

constexpr float kRadius = kCorner;

// Is this background light enough to need dark text?
bool light(juce::Colour c) { return c.getPerceivedBrightness() > 0.62f; }

} // namespace

// Transport icons, centred in the area, about as tall as its height.
void drawIcon(juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area) {
  const float s = std::min(area.getWidth(), area.getHeight()) * 0.5f;
  const auto c = area.getCentre();
  juce::Path p;
  if (name == "play") {
    p.addTriangle(c.x - s * 0.4f, c.y - s * 0.5f, c.x - s * 0.4f, c.y + s * 0.5f, c.x + s * 0.5f, c.y);
    g.fillPath(p);
  } else if (name == "pause") {  // two bars
    for (float x : {-0.4f, 0.12f}) g.fillRoundedRectangle(c.x + s * x, c.y - s * 0.5f, s * 0.28f, s, 1.0f);
  } else if (name == "stop") {
    g.fillRoundedRectangle(juce::Rectangle<float>(s * 0.8f, s * 0.8f).withCentre(c), 1.5f);
  } else if (name == "home") {  // back to the start: a bar and a triangle pointing at it
    g.fillRect(juce::Rectangle<float>(c.x - s * 0.5f, c.y - s * 0.45f, s * 0.16f, s * 0.9f));
    p.addTriangle(c.x + s * 0.45f, c.y - s * 0.45f, c.x + s * 0.45f, c.y + s * 0.45f, c.x - s * 0.25f, c.y);
    g.fillPath(p);
  } else if (name == "undo" || name == "redo") {  // a curved arrow, back (undo) or forward (redo)
    const float dir = name == "undo" ? 1.0f : -1.0f, pi = juce::MathConstants<float>::pi;
    p.addCentredArc(c.x, c.y + s * 0.15f, s * 0.55f, s * 0.45f, 0, dir * -pi * 0.5f, dir * pi * 0.5f, true);
    g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const auto tip = juce::Point<float>(c.x - dir * s * 0.55f, c.y + s * 0.15f);
    juce::Path head;
    head.addTriangle(tip.x - 3.5f, tip.y - 1.0f, tip.x + 3.5f, tip.y - 1.0f, tip.x, tip.y + 4.0f);  // pointing down at the arc's end
    g.fillPath(head);
  } else if (name == "loop") {  // two arcs chasing each other, each with its arrowhead
    const float r = s * 0.5f, pi = juce::MathConstants<float>::pi;
    p.addCentredArc(c.x, c.y, r * 1.15f, r * 0.8f, 0, pi * 0.15f, pi * 0.85f, true);
    p.addCentredArc(c.x, c.y, r * 1.15f, r * 0.8f, 0, pi * 1.15f, pi * 1.85f, true);
    g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (float a : {pi * 0.85f, pi * 1.85f}) {
      const auto tip = juce::Point<float>(c.x + r * 1.15f * std::sin(a), c.y - r * 0.8f * std::cos(a));
      const float dir = a < pi ? 1.0f : -1.0f;  // the lower arc runs left, the upper one right
      juce::Path head;
      head.addTriangle(tip.x - dir * 3.5f, tip.y - 3.0f, tip.x - dir * 3.5f, tip.y + 3.0f, tip.x + dir * 2.0f, tip.y);
      g.fillPath(head);
    }
  } else if (name == "pencil") {  // a slanted pencil
    juce::Path body;
    body.startNewSubPath(c.x - s * 0.55f, c.y + s * 0.55f);
    body.lineTo(c.x - s * 0.45f, c.y + s * 0.2f);
    body.lineTo(c.x + s * 0.3f, c.y - s * 0.55f);
    body.lineTo(c.x + s * 0.55f, c.y - s * 0.3f);
    body.lineTo(c.x - s * 0.2f, c.y + s * 0.45f);
    body.closeSubPath();
    g.fillPath(body);
  } else if (name == "select") {  // a dashed square
    const auto r = juce::Rectangle<float>(s * 1.1f, s * 1.1f).withCentre(c);
    const float dashes[] = {2.5f, 2.0f};
    for (auto line : {juce::Line<float>(r.getTopLeft(), r.getTopRight()), juce::Line<float>(r.getTopRight(), r.getBottomRight()),
                      juce::Line<float>(r.getBottomRight(), r.getBottomLeft()), juce::Line<float>(r.getBottomLeft(), r.getTopLeft())})
      g.drawDashedLine(line, dashes, 2, 1.3f);
  } else if (name == "erase") {  // a tilted eraser
    juce::Path e;
    e.addRoundedRectangle(-s * 0.55f, -s * 0.28f, s * 1.1f, s * 0.56f, 1.5f);
    e.applyTransform(juce::AffineTransform::rotation(-0.75f).translated(c.x, c.y));
    g.strokePath(e, juce::PathStrokeType(1.4f));
    g.drawLine(c.x - s * 0.1f, c.y - s * 0.45f, c.x + s * 0.45f, c.y + s * 0.1f, 1.4f);
  } else if (name == "folder") {
    juce::Path f;
    f.startNewSubPath(c.x - s * 0.6f, c.y + s * 0.45f);
    f.lineTo(c.x - s * 0.6f, c.y - s * 0.45f);
    f.lineTo(c.x - s * 0.15f, c.y - s * 0.45f);
    f.lineTo(c.x, c.y - s * 0.28f);
    f.lineTo(c.x + s * 0.6f, c.y - s * 0.28f);
    f.lineTo(c.x + s * 0.6f, c.y + s * 0.45f);
    f.closeSubPath();
    g.strokePath(f, juce::PathStrokeType(1.3f, juce::PathStrokeType::mitered));
  } else if (name == "star" || name == "starOn") {
    juce::Path st;
    st.addStar(c, 5, s * 0.28f, s * 0.62f, 0);
    if (name == "starOn") g.fillPath(st);
    else g.strokePath(st, juce::PathStrokeType(1.2f));
  } else if (name == "wave") {  // a small waveform: bars of different heights
    const float hs[] = {0.25f, 0.6f, 0.9f, 0.5f, 0.75f, 0.35f, 0.2f};
    const float w = s * 1.3f / 7.0f;
    for (int i = 0; i < 7; ++i) g.fillRect(c.x - s * 0.65f + i * w, c.y - s * hs[i] * 0.55f, std::max(1.0f, w * 0.55f), s * hs[i] * 1.1f);
  } else if (name == "down" || name == "right" || name == "left" || name == "up") {
    // A chevron: two strokes meeting at a point, centred, 2 px with round ends so it stays clean at any size
    // (Diogo 2026-11-09: "triângulo mal feito", "difícil de ver").
    const float a = s * 0.6f, b = s * 0.32f;
    juce::Path ch;
    if (name == "down") { ch.startNewSubPath(c.x - a, c.y - b); ch.lineTo(c.x, c.y + b); ch.lineTo(c.x + a, c.y - b); }
    else if (name == "up") { ch.startNewSubPath(c.x - a, c.y + b); ch.lineTo(c.x, c.y - b); ch.lineTo(c.x + a, c.y + b); }
    else if (name == "right") { ch.startNewSubPath(c.x - b, c.y - a); ch.lineTo(c.x + b, c.y); ch.lineTo(c.x - b, c.y + a); }
    else { ch.startNewSubPath(c.x + b, c.y - a); ch.lineTo(c.x - b, c.y); ch.lineTo(c.x + b, c.y + a); }
    g.strokePath(ch, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
  } else if (name == "cursor") {  // the pointer arrow (Diogo 2026-11-09: "trocar lápis por um cursor")
    juce::Path arrow;
    const float x = c.x - s * 0.35f, y = c.y - s * 0.6f;
    arrow.startNewSubPath(x, y);
    arrow.lineTo(x, y + s * 1.05f);
    arrow.lineTo(x + s * 0.28f, y + s * 0.8f);
    arrow.lineTo(x + s * 0.48f, y + s * 1.2f);
    arrow.lineTo(x + s * 0.64f, y + s * 1.12f);
    arrow.lineTo(x + s * 0.44f, y + s * 0.73f);
    arrow.lineTo(x + s * 0.8f, y + s * 0.73f);
    arrow.closeSubPath();
    g.fillPath(arrow);
  } else if (name == "search") {
    g.drawEllipse(juce::Rectangle<float>(s * 0.8f, s * 0.8f).withCentre({c.x - s * 0.12f, c.y - s * 0.12f}), 1.4f);
    g.drawLine(c.x + s * 0.18f, c.y + s * 0.18f, c.x + s * 0.5f, c.y + s * 0.5f, 1.6f);
  } else if (name == "library") {  // books on a shelf
    for (int i = 0; i < 3; ++i) g.drawRect(juce::Rectangle<float>(s * 0.3f, s * 1.1f).withCentre({c.x - s * 0.4f + i * s * 0.4f, c.y}), 1.2f);
  } else if (name == "inspector") {  // three slider lines
    for (int i = 0; i < 3; ++i) {
      const float y = c.y - s * 0.4f + i * s * 0.4f;
      g.drawLine(c.x - s * 0.55f, y, c.x + s * 0.55f, y, 1.2f);
      g.fillEllipse(juce::Rectangle<float>(s * 0.3f, s * 0.3f).withCentre({c.x + (i == 1 ? 0.25f : -0.25f) * s, y}));
    }  } else if (name == "arrange") {  // clips on lanes: blocks of different lengths in three rows
    const float h = s * 0.32f;
    const float rows[3][2] = {{-0.6f, 0.2f}, {-0.3f, 0.6f}, {-0.6f, -0.05f}};
    for (int i = 0; i < 3; ++i)
      g.fillRoundedRectangle(c.x + rows[i][0] * s, c.y - s * 0.55f + i * s * 0.42f, (rows[i][1] - rows[i][0]) * s, h, 1.0f);
    g.fillRoundedRectangle(c.x + 0.3f * s, c.y - s * 0.55f, 0.3f * s, h, 1.0f);
  } else if (name == "plugins") {  // the plugins' cards: four squares, one filled
    const float w = s * 0.5f;
    for (int i = 0; i < 4; ++i) {
      const auto r = juce::Rectangle<float>(w, w).withCentre({c.x + (i % 2 ? 0.32f : -0.32f) * s, c.y + (i / 2 ? 0.32f : -0.32f) * s});
      if (i == 0) g.fillRoundedRectangle(r, 1.5f); else g.drawRoundedRectangle(r, 1.5f, 1.2f);
    }
  } else if (name == "patterns") {  // the channel rack: three rows of steps, some on
    const float w = s * 0.26f, gap = s * 0.1f;
    const bool on[3][4] = {{true, false, true, false}, {false, true, false, true}, {true, true, false, true}};
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 4; ++col) {
        const auto r = juce::Rectangle<float>(w, w).withPosition(c.x - 2 * w - 1.5f * gap + col * (w + gap), c.y - 1.5f * w - gap + row * (w + gap));
        if (on[row][col]) g.fillRoundedRectangle(r, 1.0f); else g.drawRoundedRectangle(r, 1.0f, 1.0f);
      }
  } else if (name == "mixer") {  // three faders: vertical lines with their caps at different heights
    for (int i = 0; i < 3; ++i) {
      const float x = c.x - s * 0.45f + i * s * 0.45f, cap = c.y + (i == 0 ? 0.2f : i == 1 ? -0.3f : 0.05f) * s;
      g.drawLine(x, c.y - s * 0.6f, x, c.y + s * 0.6f, 1.2f);
      g.fillRoundedRectangle(juce::Rectangle<float>(s * 0.34f, s * 0.2f).withCentre({x, cap}), 1.0f);
    }
  }
}

juce::Path logoPath(juce::Rectangle<float> area) {
  // Drawn on a 400 x 480 grid (the reference logo), then scaled to fit. Three pieces make the C and leave the
  // gaps between them; the diamond sits in its middle.
  juce::Path p;
  auto poly = [&p](std::initializer_list<juce::Point<float>> points) {
    bool first = true;
    for (auto pt : points) {
      if (first) p.startNewSubPath(pt);
      else p.lineTo(pt);
      first = false;
    }
    p.closeSubPath();
  };
  // Upper arm and the left side, one piece.
  poly({{0, 135}, {210, 0}, {283, 52}, {86, 180}, {86, 315}, {0, 360}});
  // The short upper-right piece.
  poly({{238, 125}, {313, 75}, {393, 130}, {318, 178}});
  // The lower arm, a wide V.
  poly({{43, 380}, {108, 330}, {206, 388}, {328, 315}, {393, 350}, {206, 478}});
  // The core.
  poly({{208, 200}, {256, 248}, {208, 295}, {160, 248}});
  p.applyTransform(p.getTransformToScaleToFit(area, true));
  return p;
}

void drawLogo(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour) {
  g.setColour(colour);
  g.fillPath(logoPath(area));
}

void drawWordmark(juce::Graphics& g, juce::Rectangle<float> area) {
  area = area.toNearestInt().toFloat();  // whole pixels: the C's straight edges stay crisp
  const float h = area.getHeight();
  drawLogo(g, area.removeFromLeft(h * 0.84f).reduced(0, h * 0.04f), kText);
  area.removeFromLeft(h * 0.36f);
  g.setColour(kText);
  // A sans named outright: the default bold face on Windows came out with serifs.
#if JUCE_WINDOWS
  g.setFont(juce::FontOptions("Segoe UI", h * 0.55f, juce::Font::bold).withKerningFactor(0.18f));
#else
  g.setFont(juce::FontOptions(h * 0.55f, juce::Font::bold).withKerningFactor(0.18f));
#endif
  g.drawText("CORE", area, juce::Justification::centredLeft, false);
}

void setIdentity(juce::Component& c, juce::Colour colour) {
  auto& props = c.getProperties();
  const bool had = props.contains(kIdentityProperty);
  if (colour.isTransparent()) props.remove(kIdentityProperty);
  else props.set(kIdentityProperty, (juce::int64)colour.getARGB());
  if (had || !colour.isTransparent()) c.repaint();
}

juce::Colour identityOf(const juce::Component& c, juce::Colour fallback) {
  for (const juce::Component* p = &c; p != nullptr; p = p->getParentComponent())
    if (const auto* v = p->getProperties().getVarPointer(kIdentityProperty)) return juce::Colour((juce::uint32)(juce::int64)*v);
  return fallback;
}

void paintCard(juce::Graphics& g, juce::Rectangle<float> r, float headerH, juce::Colour identity, float alpha, bool selected) {
  constexpr float corner = kCorner;
  const bool tinted = !identity.isTransparent();
  g.setColour(kCard.withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(r, corner);
  if (headerH > 0.0f) {
    const auto band = r.withHeight(std::min(headerH, r.getHeight()));
    juce::Path head;
    head.addRoundedRectangle(band.getX(), band.getY(), band.getWidth(), band.getHeight(), corner, corner, true, true, false, false);
    g.setColour((tinted ? kBlueDeep.interpolatedWith(identity, 0.22f) : kBlueDeep).withMultipliedAlpha(alpha));
    g.fillPath(head);
    if (tinted) {
      juce::Graphics::ScopedSaveState state(g);
      g.reduceClipRegion(head);
      g.setColour(identity.withMultipliedAlpha(alpha));
      g.fillRect(band.withHeight(3.0f));
    }
    g.setColour(kOutline.withMultipliedAlpha(alpha));
    g.fillRect(band.getX() + 1.0f, band.getBottom() - 1.0f, band.getWidth() - 2.0f, 1.0f);
  }
  // The neutral outline; only the selected card gets a line in its identity colour, 2 px (Diogo 2026-11-08).
  const juce::Colour line = !selected ? kOutline : tinted ? identity : kBlueSoft;
  g.setColour(line.withMultipliedAlpha(alpha));
  const float w = selected ? 2.0f : 1.0f;
  g.drawRoundedRectangle(r.reduced(w / 2), corner, w);
}

void paintSectionTitle(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, juce::Colour mark) {
  // The square look: a small square mark and the title in capitals, like MODULATION MATRIX in the brief.
  g.setColour(mark);
  g.fillRect(juce::Rectangle<float>(5.0f, 5.0f).withPosition(r.getX(), r.getCentreY() - 2.5f));
  g.setColour(kBlueLight);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.06f));
  g.drawText(lang::tr(title).toUpperCase(), r.withTrimmedLeft(12.0f), juce::Justification::centredLeft, true);
}

juce::Colour toneScale(juce::Colour base, int count, int index) {
  // Lightness from 0.80 (top) to 0.42 (bottom); the hue and (a touch more) saturation of the base.
  const float t = count <= 1 ? 0.0f : juce::jlimit(0.0f, 1.0f, (float)index / (float)(count - 1));
  const float hue = base.getHue(), sat = std::min(1.0f, base.getSaturationHSL() * 1.05f);
  return juce::Colour::fromHSL(hue, sat, 0.80f - 0.38f * t, 1.0f);
}

juce::Colour neutralScale(int count, int index) {
  static const juce::Colour steps[] = {kBlueLight, kBlueSoft, kBlueBase, kBlueHeading};
  if (count <= 1) return steps[0];
  const float t = juce::jlimit(0.0f, 1.0f, (float)index / (float)(count - 1)) * 3.0f;
  const int i = std::min(2, (int)t);
  return steps[i].interpolatedWith(steps[i + 1], t - (float)i);
}

void paintToneSection(juce::Graphics& g, juce::Rectangle<float> r, float headerH, juce::Colour tone, const juce::String& title, float alpha) {
  g.setColour(kCard.withMultipliedAlpha(alpha));
  g.fillRect(r);
  if (headerH > 0.0f) {
    const auto head = r.withHeight(std::min(headerH, r.getHeight()));
    g.setColour(kCard.interpolatedWith(tone, 0.35f).withMultipliedAlpha(alpha));
    g.fillRect(head);
    if (title.isNotEmpty()) {
      g.setColour(tone.interpolatedWith(kText, 0.45f).withMultipliedAlpha(alpha));
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.06f));
      g.drawText(lang::tr(title).toUpperCase(), head.reduced(10.0f, 0.0f), juce::Justification::centredLeft, true);
    }
  }
  g.setColour(tone.withMultipliedAlpha(alpha));
  g.drawRect(r, 2.0f);
}

LookAndFeel::LookAndFeel() {
#if JUCE_WINDOWS
  // Windows' own interface face for every text in Core: JUCE's default sans there is thin and blurry at small
  // sizes (and its bold came out with serifs), Segoe UI is drawn for the screen.
  setDefaultSansSerifTypefaceName("Segoe UI");
#endif
  auto scheme = getDarkColourScheme();
  using CS = juce::LookAndFeel_V4::ColourScheme;
  scheme.setUIColour(CS::windowBackground, kBackground);
  scheme.setUIColour(CS::widgetBackground, kControl);
  scheme.setUIColour(CS::menuBackground, kPanel);
  scheme.setUIColour(CS::outline, kOutline);
  scheme.setUIColour(CS::defaultText, kText);
  scheme.setUIColour(CS::defaultFill, kAccent);
  scheme.setUIColour(CS::highlightedText, kText);
  scheme.setUIColour(CS::highlightedFill, kControlHover);
  scheme.setUIColour(CS::menuText, kText);
  setColourScheme(scheme);

  setColour(juce::ResizableWindow::backgroundColourId, kBackground);
  setColour(juce::DocumentWindow::textColourId, kText);
  setColour(juce::TextButton::buttonColourId, kControl);
  setColour(juce::TextButton::buttonOnColourId, kPrimary);
  setColour(juce::TextButton::textColourOffId, kText);
  setColour(juce::TextButton::textColourOnId, kText);
  setColour(juce::ComboBox::backgroundColourId, kField);
  setColour(juce::ComboBox::outlineColourId, kOutline);
  setColour(juce::ComboBox::textColourId, kText);
  setColour(juce::ComboBox::arrowColourId, kMuted);
  setColour(juce::ComboBox::focusedOutlineColourId, kPrimaryLight);
  setColour(juce::TextEditor::backgroundColourId, kField);
  setColour(juce::TextEditor::textColourId, kText);
  setColour(juce::TextEditor::outlineColourId, kOutline);
  setColour(juce::TextEditor::focusedOutlineColourId, kPrimaryLight);
  setColour(juce::TextEditor::highlightColourId, kPrimaryLight.withAlpha(0.45f));
  setColour(juce::CaretComponent::caretColourId, kAccentBright);
  setColour(juce::Label::textColourId, kText);
  setColour(juce::Slider::backgroundColourId, kField);
  setColour(juce::Slider::trackColourId, kAccent);
  setColour(juce::Slider::thumbColourId, kText);
  setColour(juce::Slider::rotarySliderFillColourId, kAccent);
  setColour(juce::Slider::rotarySliderOutlineColourId, kField);
  setColour(juce::Slider::textBoxTextColourId, kText);
  setColour(juce::Slider::textBoxBackgroundColourId, kField);
  setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  setColour(juce::Slider::textBoxHighlightColourId, kAccent.withAlpha(0.35f));
  setColour(juce::PopupMenu::backgroundColourId, kPanel);
  setColour(juce::PopupMenu::textColourId, kText);
  setColour(juce::PopupMenu::headerTextColourId, kMuted);
  setColour(juce::PopupMenu::highlightedBackgroundColourId, kPrimary);
  setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
  setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff3a4450));
  setColour(juce::TooltipWindow::backgroundColourId, kCard);
  setColour(juce::TooltipWindow::textColourId, kText);
  setColour(juce::TooltipWindow::outlineColourId, kOutline);
  setColour(juce::AlertWindow::backgroundColourId, kPanel);
  setColour(juce::AlertWindow::textColourId, kText);
  setColour(juce::AlertWindow::outlineColourId, kOutline);
  setColour(juce::ListBox::backgroundColourId, kField);
  setColour(juce::ListBox::textColourId, kText);
  setColour(juce::ListBox::outlineColourId, kOutline);
  setColour(juce::ProgressBar::backgroundColourId, kField);
  setColour(juce::ProgressBar::foregroundColourId, kAccent);
  setColour(juce::ToggleButton::textColourId, kText);
  setColour(juce::ToggleButton::tickColourId, kPrimaryLight);
  setColour(juce::ToggleButton::tickDisabledColourId, kDim);
  setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
}

// Buttons (Diogo 2026-11-09: "deveria estar escuro ou claro e ter comportamento inverso ao carregar, hover ou acionar"):
// two states only, dark (off) and light (on), and the mouse inverts them: a dark button turns light under the mouse
// and while pressed, a light one turns dark. The text is always the opposite of the fill, so it reads. Square corners,
// no highlight blue, no glow. A button in a colour of its own (a track's, a warning's) keeps it, a tone lighter under
// the mouse.
namespace {
bool themed(juce::Colour c) { return c.isTransparent() || c == kControl || c == kPrimary || c == kBlueLight; }
juce::Colour buttonFill(const juce::Button& b, juce::Colour background, bool over, bool down) {
  const bool on = b.getToggleState();
  if (!themed(background)) return over || down ? background.brighter(down ? 0.2f : 0.1f) : background;
  const bool lit = on != (over || down);  // the mouse inverts
  if (lit) return down ? kBlueLight.darker(0.12f) : kBlueLight;
  if (background.isTransparent() && !on) return background;  // a tab: no fill until the mouse is on it
  return kButtonDark;  // never an empty box (Diogo 2026-11-09: "não quero botões vazios")
}
}  // namespace

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& background, bool over, bool down) {
  auto r = b.getLocalBounds().toFloat().reduced(0.5f);
  juce::Colour fill = buttonFill(b, background, over, down);
  const bool enabled = b.isEnabled();
  if (!enabled) { fill = b.getToggleState() ? kBlueSoft : (fill.isTransparent() ? fill : kButtonDark); fill = fill.withMultipliedAlpha(0.5f); }
  // Buttons joined to a neighbour (a stepper's − and +) keep square corners on that side.
  const bool left = !b.isConnectedOnLeft(), right = !b.isConnectedOnRight();
  juce::Path shape;
  shape.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(), kRadius, kRadius, left, right, left, right);
  // A solid block, dark or light, with no outline: the fill alone says what the button is.
  g.setColour(fill);
  g.fillPath(shape);
}

void LookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool over, bool down) {
  const bool on = b.getToggleState();
  const juce::Colour background = b.findColour(on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
  const juce::Colour fill = buttonFill(b, background, over && b.isEnabled(), down && b.isEnabled());
  juce::Colour text;
  if (light(fill)) text = kBackground;                                   // dark words on a light button
  else if (!themed(background)) text = juce::Colours::white;             // a button in its own colour
  else if (on) text = kText;
  else text = b.findColour(juce::TextButton::textColourOffId);
  if (!b.isEnabled()) text = kDim;
  g.setColour(text);
  if (const auto icon = b.getProperties()[kIconProperty].toString(); icon.isNotEmpty()) {
    drawIcon(g, icon, b.getLocalBounds().toFloat().reduced(2.0f));
    return;
  }
  g.setFont(getTextButtonFont(b, b.getHeight()));
  const juce::String words = lang::tr(b.getButtonText());
  // Small buttons (M, S, FX on a track) keep almost all their width for the letter.
  const int pad = std::min(8, b.getWidth() / 8);
  g.drawFittedText(words, b.getLocalBounds().reduced(pad, 2), juce::Justification::centred, 2, 0.7f);
}

int LookAndFeel::getTextButtonWidthToFitText(juce::TextButton& b, int buttonHeight) {
  return juce::GlyphArrangement::getStringWidthInt(getTextButtonFont(b, buttonHeight), lang::tr(b.getButtonText())) + buttonHeight;
}

void LookAndFeel::drawPlainLabel(juce::Graphics& g, juce::Label& label) {
  g.fillAll(label.findColour(juce::Label::backgroundColourId));
  const float alpha = label.isEnabled() ? 1.0f : 0.5f;
  if (!label.isBeingEdited()) {
    const juce::Font font(getLabelFont(label));
    const bool typed = label.isEditableOnSingleClick() || label.isEditableOnDoubleClick();
    g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
    g.setFont(font);
    const auto area = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
    g.drawFittedText(typed ? label.getText() : lang::tr(label.getText()), area, label.getJustificationType(),
                     juce::jmax(1, (int)((float)area.getHeight() / font.getHeight())), label.getMinimumHorizontalScale());
  }
  g.setColour(label.findColour(juce::Label::outlineColourId).withMultipliedAlpha(label.isBeingEdited() ? 1.0f : alpha));
  g.drawRect(label.getLocalBounds());
}

void LookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool over, bool down) {
  const float fontSize = juce::jmin(15.0f, (float)button.getHeight() * 0.75f);
  const float tickWidth = fontSize * 1.1f;
  drawTickBox(g, button, 4.0f, ((float)button.getHeight() - tickWidth) * 0.5f, tickWidth, tickWidth, button.getToggleState(),
              button.isEnabled(), over, down);
  g.setColour(button.findColour(juce::ToggleButton::textColourId));
  g.setFont(fontSize);
  if (!button.isEnabled()) g.setOpacity(0.5f);
  g.drawFittedText(lang::tr(button.getButtonText()), button.getLocalBounds().withTrimmedLeft(juce::roundToInt(tickWidth) + 10).withTrimmedRight(2),
                   juce::Justification::centredLeft, 10);
}

void LookAndFeel::changeToggleButtonWidthToFitText(juce::ToggleButton& button) {
  const float fontSize = juce::jmin(15.0f, (float)button.getHeight() * 0.75f);
  const float tickWidth = fontSize * 1.1f;
  const juce::Font font(juce::FontOptions{fontSize});
  button.setSize(juce::GlyphArrangement::getStringWidthInt(font, lang::tr(button.getButtonText())) + juce::roundToInt(tickWidth) + 14,
                 button.getHeight());
}

// Every dialog made from options (message boxes, OK / Cancel questions) in Core's language.
std::unique_ptr<juce::AlertWindow> LookAndFeel::createAlertWindowForOptions(const juce::MessageBoxOptions& opt) {
  auto words = juce::MessageBoxOptions()
                   .withIconType(opt.getIconType())
                   .withTitle(lang::tr(opt.getTitle()))
                   .withMessage(lang::tr(opt.getMessage()))
                   .withAssociatedComponent(opt.getAssociatedComponent())
                   .withParentComponent(opt.getParentComponent());
  for (const auto& b : opt.getButtons())
    words = words.withButton(juce::MessageBoxButtonOptions(lang::tr(b.getText()), b.getCode()).withRole(b.getRole()).withPreferredAction(b.isPreferredAction()));
  return juce::LookAndFeel_V4::createAlertWindowForOptions(words);
}

juce::Font LookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight) {
  return juce::FontOptions(std::min(kFontBody, buttonHeight * 0.6f));  // regular weight, as on the references
}

// Value bars (volume, tempo) are a dark field with the accent filling up to the value; a plain horizontal slider
// is a thin track with a round white thumb, as on the reference's Gain.
void LookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& textLayout) {
  const auto bounds = alert.getLocalBounds().toFloat();
  g.setColour(kCard);
  g.fillRoundedRectangle(bounds, kCorner);
  g.setColour(kOutline);
  g.drawRoundedRectangle(bounds.reduced(0.5f), kCorner, 1.0f);
  const auto type = alert.getAlertType();
  if (type != juce::MessageBoxIconType::NoIcon) {
    const bool warning = type == juce::MessageBoxIconType::WarningIcon;
    const juce::Colour colour = warning ? kWarn : kPrimaryLight;
    // In the room JUCE keeps for its icon (left of the text), level with the title.
    const auto mark = juce::Rectangle<float>(28, 28).withCentre({(float)textArea.getX() / 2.0f + 4.0f, (float)textArea.getY() + 14.0f});
    g.setColour(colour.withAlpha(0.18f));
    g.fillEllipse(mark);
    g.setColour(colour);
    g.drawEllipse(mark.reduced(0.75f), 1.5f);
    g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    g.drawText(warning ? "!" : type == juce::MessageBoxIconType::QuestionIcon ? "?" : "i", mark, juce::Justification::centred, false);
  }
  g.setColour(alert.findColour(juce::AlertWindow::textColourId));
  textLayout.draw(g, textArea.toFloat());
}

void LookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float pos, float minPos, float maxPos,
                                   juce::Slider::SliderStyle style, juce::Slider& s) {
  const auto r = juce::Rectangle<int>(x, y, width, height).toFloat();
  const float alpha = s.isEnabled() ? 1.0f : 0.45f;
  const bool live = s.isEnabled() && (s.isMouseOverOrDragging());
  const bool level = (bool)s.getProperties()[kLevelProperty];
  const juce::Colour ident = level ? kBlueSoft : identityOf(s),
                     identBright = level ? kBlueLight : ident == kAccent ? kAccentBright : ident.brighter(0.25f);
  if (style == juce::Slider::LinearBar || style == juce::Slider::LinearBarVertical) {
    // A value bar (send level, tempo, volume): a dark field, the value as a soft fill and as an accent line on a
    // dim track along the bottom edge. The number sits in the middle at a size that fits the bar
    // (getLabelFont), so nothing crosses it.
    const auto box = s.getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(kField.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(box, kRadius);
    const bool across = style == juce::Slider::LinearBar;
    if ((bool)s.getProperties()[kPlainProperty]) {  // a plain field: the number only
      g.setColour((live ? kBlueSoft : kBlueHeading).withMultipliedAlpha(alpha));
      g.drawRoundedRectangle(box, kRadius, 1.0f);
      return;
    }
    const auto fill = across ? box.withRight(juce::jlimit(box.getX(), box.getRight(), pos))
                             : box.withTop(juce::jlimit(box.getY(), box.getBottom(), pos));
    {
      juce::Graphics::ScopedSaveState state(g);
      juce::Path clip;
      clip.addRoundedRectangle(box, kRadius);
      g.reduceClipRegion(clip);
      g.setColour(ident.withAlpha((live ? 0.62f : 0.5f) * alpha));  // the track's or group's colour
      g.fillRect(fill);
      g.setColour((live ? identBright : ident).withMultipliedAlpha(alpha));
      if (across) {
        g.setColour(kOutline.withMultipliedAlpha(alpha * 0.8f));
        g.fillRect(box.getX(), box.getBottom() - 1.5f, box.getWidth(), 1.5f);
        g.setColour((live ? identBright : ident).withMultipliedAlpha(alpha));
        g.fillRect(box.getX(), box.getBottom() - 1.5f, fill.getWidth(), 1.5f);
      } else if (fill.getHeight() > 1.0f) {
        g.fillRect(box.getX(), fill.getY(), box.getWidth(), 1.5f);
      }
    }
    g.setColour((live ? kControlHover.brighter(0.25f) : kOutline).withMultipliedAlpha(alpha));
    g.drawRoundedRectangle(box, kRadius, 1.0f);
    return;
  }
  if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical) {
    LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, pos, minPos, maxPos, style, s);
    return;
  }
  const bool horizontal = style == juce::Slider::LinearHorizontal;
  const float thick = 4.0f;
  const juce::Point<float> start = horizontal ? juce::Point<float>(r.getX(), r.getCentreY()) : juce::Point<float>(r.getCentreX(), r.getBottom());
  const juce::Point<float> end = horizontal ? juce::Point<float>(r.getRight(), r.getCentreY()) : juce::Point<float>(r.getCentreX(), r.getY());
  const juce::Point<float> at = horizontal ? juce::Point<float>(pos, r.getCentreY()) : juce::Point<float>(r.getCentreX(), pos);
  const juce::PathStrokeType stroke(thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
  juce::Path track, value;
  track.startNewSubPath(start);
  track.lineTo(end);
  value.startNewSubPath(start);
  value.lineTo(at);
  g.setColour(kField.brighter(0.2f).withMultipliedAlpha(alpha));
  g.strokePath(track, stroke);
  g.setColour((live ? identBright : ident).withMultipliedAlpha(alpha));
  g.strokePath(value, stroke);
  if (horizontal) {
    // A round thumb with a soft shadow (no halo on hover: the value line just turns one step lighter).
    const float d = 13.0f;
    const auto thumb = juce::Rectangle<float>(d, d).withCentre(at);
    g.setColour(juce::Colours::black.withAlpha(0.35f * alpha));
    g.fillEllipse(thumb.translated(0.0f, 1.0f));
    g.setGradientFill(juce::ColourGradient(kText.withMultipliedAlpha(alpha), thumb.getCentreX(), thumb.getY(), kTitle.withMultipliedAlpha(alpha),
                                           thumb.getCentreX(), thumb.getBottom(), false));
    g.fillEllipse(thumb);
    return;
  }
  // A vertical fader: a small cap with a line across its middle, as on a mixing desk.
  const auto cap = juce::Rectangle<float>(std::min(24.0f, r.getWidth() - 2.0f), 14.0f).withCentre(at);
  g.setColour(juce::Colours::black.withAlpha(0.4f * alpha));
  g.fillRoundedRectangle(cap.translated(0.0f, 1.5f), kCorner);
  g.setGradientFill(juce::ColourGradient((live ? kText : kTitle).withMultipliedAlpha(alpha), cap.getCentreX(), cap.getY(),
                                         kMuted.withMultipliedAlpha(alpha), cap.getCentreX(), cap.getBottom(), false));
  g.fillRoundedRectangle(cap, kCorner);
  g.setColour(kBackground.withAlpha(0.8f * alpha));
  g.fillRect(cap.getX() + 3.0f, cap.getCentreY() - 0.75f, cap.getWidth() - 6.0f, 1.5f);
}

// Knobs: a ring with the value as an accent arc (from the middle for values that go both ways, such as pan or
// an EQ band's dB), a softly shaded body with a short shadow, and a rounded pointer. Hover or a drag brightens
// the arc. Every size, from the mixer's small pan to an effect's knob, is drawn in the same proportions.
void LookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float pos, float startAngle, float endAngle,
                                   juce::Slider& s) {
  const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(2.0f);
  const float radius = std::min(bounds.getWidth(), bounds.getHeight()) / 2;
  if (radius < 4.0f) return;
  const auto centre = bounds.getCentre();
  const float angle = startAngle + pos * (endAngle - startAngle);
  const float alpha = s.isEnabled() ? 1.0f : 0.45f;
  const bool live = s.isEnabled() && s.isMouseOverOrDragging();
  const float ring = juce::jlimit(1.5f, 3.0f, radius * 0.11f);
  const float arcR = radius - ring / 2;
  const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
  const float from = bipolar ? startAngle + (float)s.valueToProportionOfLength(0.0) * (endAngle - startAngle) : startAngle;
  // The ring and the value are filled bands (a closed outline with round ends), not stroked lines: on Windows'
  // Direct2D a stroked arc of many short segments came out shorter than the pointer, so the arc and the
  // pointer told different values. A filled shape is drawn the same everywhere.
  const auto band = [&](float a0, float a1) {
    juce::Path p;
    const float rOut = arcR + ring / 2, rIn = arcR - ring / 2, capR = ring / 2;
    const int n = std::max(2, (int)std::ceil(std::abs(a1 - a0) / 0.04f));
    for (int i = 0; i <= n; ++i) {
      const auto pt = centre.getPointOnCircumference(rOut, a0 + (a1 - a0) * (float)i / (float)n);
      if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
    }
    for (int i = n; i >= 0; --i) p.lineTo(centre.getPointOnCircumference(rIn, a0 + (a1 - a0) * (float)i / (float)n));
    p.closeSubPath();
    for (const float a : {a0, a1}) {
      const auto c = centre.getPointOnCircumference(arcR, a);
      p.addEllipse(c.x - capR, c.y - capR, capR * 2, capR * 2);
    }
    p.setUsingNonZeroWinding(true);
    return p;
  };
  g.setColour(kField.brighter(0.22f).withMultipliedAlpha(alpha));
  g.fillPath(band(startAngle, endAngle));
  if (std::abs(angle - from) > 0.01f) {
    // An arc coloured on the slider itself wins; otherwise the colour of the track or group it belongs to.
    const juce::Colour arc = s.isColourSpecified(juce::Slider::rotarySliderFillColourId) ? s.findColour(juce::Slider::rotarySliderFillColourId)
                                                                                        : identityOf(s);
    g.setColour((live ? (arc == kAccent ? kAccentBright : arc.brighter(0.25f)) : arc).withMultipliedAlpha(alpha));
    g.fillPath(band(std::min(from, angle), std::max(from, angle)));
  }
  // The body: a gap inside the ring, a shadow under it, lighter at the top.
  const float body = radius - ring - std::max(2.0f, radius * 0.1f);
  const auto disc = juce::Rectangle<float>(body * 2, body * 2).withCentre(centre);
  g.setColour(juce::Colours::black.withAlpha(0.35f * alpha));
  g.fillEllipse(disc.translated(0.0f, std::max(1.0f, body * 0.06f)));
  // A dark charcoal body, a touch lighter at the top (and one step lighter under the mouse); the arc carries the colour.
  g.setGradientFill(juce::ColourGradient((live ? juce::Colour(0xff35404f) : juce::Colour(0xff2b3542)).withMultipliedAlpha(alpha),
                                         centre.x, disc.getY(), juce::Colour(0xff141b23).withMultipliedAlpha(alpha), centre.x, disc.getBottom(), false));
  g.fillEllipse(disc);
  g.setColour(kOutline.brighter(0.1f).withMultipliedAlpha(alpha));
  g.drawEllipse(disc.reduced(0.5f), 1.0f);
  // The pointer: white, from near the centre to the edge, with rounded ends (Diogo 2026-11-09: "não vejo a ponta que
  // nos diz onde está").
  juce::Path pointer;
  pointer.startNewSubPath(centre.getPointOnCircumference(body * 0.25f, angle));
  pointer.lineTo(centre.getPointOnCircumference(body * 0.92f, angle));
  g.setColour(juce::Colours::white.withMultipliedAlpha(alpha));
  g.strokePath(pointer, juce::PathStrokeType(juce::jlimit(1.8f, 2.8f, body * 0.13f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  // The tip: a white dot on the ring where the value is, edged in the arc's colour, so the value reads at a glance.
  {
    const juce::Colour edge = s.isColourSpecified(juce::Slider::rotarySliderFillColourId) ? s.findColour(juce::Slider::rotarySliderFillColourId)
                                                                                         : identityOf(s);
    const float tipR = juce::jlimit(2.2f, 3.6f, ring * 0.95f);
    const auto tip = centre.getPointOnCircumference(arcR, angle);
    g.setColour(edge.withMultipliedAlpha(alpha));
    g.fillEllipse(tip.x - tipR - 1.0f, tip.y - tipR - 1.0f, (tipR + 1.0f) * 2, (tipR + 1.0f) * 2);
    g.setColour(juce::Colours::white.withMultipliedAlpha(alpha));
    g.fillEllipse(tip.x - tipR, tip.y - tipR, tipR * 2, tipR * 2);
  }
}

// The number under a knob or inside a value bar: a size that fits its box (it never runs over the bar's edges or
// lines), no box of its own under a knob, and brighter while the mouse is on the control.
juce::Font LookAndFeel::getLabelFont(juce::Label& label) {
  if (auto* slider = dynamic_cast<juce::Slider*>(label.getParentComponent())) {
    const float h = (float)label.getHeight();
    const bool knob = slider->isRotary();
    if ((bool)slider->getProperties()[kPlainProperty]) return juce::FontOptions(juce::jlimit(13.0f, 17.0f, h * 0.7f), juce::Font::bold);
    return juce::FontOptions(juce::jlimit(kFontSmall, knob ? kFontSmall : kFontBody, h * (knob ? 0.8f : 0.76f)));
  }
  return LookAndFeel_V4::getLabelFont(label);
}

void LookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label) {
  auto* slider = dynamic_cast<juce::Slider*>(label.getParentComponent());
  if (slider == nullptr || label.isBeingEdited()) {
    drawPlainLabel(g, label);
    return;
  }
  const bool live = slider->isEnabled() && slider->isMouseOverOrDragging();
  const auto style = slider->getSliderStyle();
  const bool bar = style == juce::Slider::LinearBar || style == juce::Slider::LinearBarVertical;
  const bool stepper = style == juce::Slider::IncDecButtons;
  const auto r = label.getLocalBounds().toFloat();
  if (stepper) {  // the number of a stepper: a field joined to its − and + buttons
    g.setColour(kField);
    g.fillRoundedRectangle(r.reduced(0.5f), kRadius);
    g.setColour(live ? kControlHover.brighter(0.25f) : kOutline);
    g.drawRoundedRectangle(r.reduced(0.5f), kRadius, 1.0f);
  } else if (!bar) {
    const auto back = label.findColour(juce::Label::backgroundColourId);
    if (!back.isTransparent() && back != kField) g.fillAll(back);  // a panel's own choice; the default field box is left out
  }
  juce::Colour text = label.findColour(juce::Label::textColourId);
  if (slider->isRotary()) text = live ? kText : text.interpolatedWith(kMuted, 0.35f);
  g.setColour(text.withMultipliedAlpha(slider->isEnabled() ? 1.0f : 0.5f));
  g.setFont(getLabelFont(label));
  const auto area = label.getBorderSize().subtractedFrom(label.getLocalBounds()).withTrimmedBottom(bar ? 1 : 0);
  g.drawFittedText(label.getText(), area, label.getJustificationType(), 1, 0.9f);
}

// The − and + of a stepper (bars, counts): two small joined buttons with real minus and plus signs.
juce::Button* LookAndFeel::createSliderButton(juce::Slider&, bool isIncrement) {
  auto* b = new juce::TextButton(isIncrement ? juce::String::fromUTF8("+") : juce::String::fromUTF8("\xe2\x88\x92"));
  b->setConnectedEdges(isIncrement ? juce::Button::ConnectedOnLeft : juce::Button::ConnectedOnRight);
  b->setWantsKeyboardFocus(false);
  return b;
}

juce::Slider::SliderLayout LookAndFeel::getSliderLayout(juce::Slider& s) {
  auto layout = LookAndFeel_V4::getSliderLayout(s);
  if (s.getSliderStyle() == juce::Slider::IncDecButtons) layout.textBoxBounds = layout.textBoxBounds.withTrimmedRight(4);  // a gap before −
  return layout;
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) {
  const auto r = juce::Rectangle<float>((float)width, (float)height).reduced(0.5f);
  g.setColour(box.findColour(juce::ComboBox::backgroundColourId).withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.5f));
  g.fillRoundedRectangle(r, kRadius);
  g.setColour(box.hasKeyboardFocus(true) ? kPrimaryLight : box.isMouseOver(true) ? kControlHover.brighter(0.15f) : kOutline);
  g.drawRoundedRectangle(r, kRadius, 1.0f);
  // A small filled caret, as on the reference.
  const float cx = (float)width - 14.0f, cy = height * 0.5f;
  juce::Path arrow;
  arrow.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
  g.setColour(box.findColour(juce::ComboBox::arrowColourId).withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.4f));
  g.fillPath(arrow);
}

juce::Font LookAndFeel::getComboBoxFont(juce::ComboBox& box) { return juce::FontOptions(std::min(kFontBody, box.getHeight() * 0.62f)); }

// A dropdown's list opens below the box, at least as wide as it, with the Fjord rows; it never covers the box
// itself or the buttons beside it (JUCE's default lays the chosen row over the box).
juce::PopupMenu::Options LookAndFeel::getOptionsForComboBoxPopupMenu(juce::ComboBox& box, juce::Label&) {
  return juce::PopupMenu::Options()
      .withTargetComponent(&box)
      .withInitiallySelectedItem(box.getSelectedId())
      .withMinimumWidth(box.getWidth())
      .withMaximumNumColumns(1)
      .withStandardItemHeight(26)
      .withPreferredPopupDirection(juce::PopupMenu::Options::PopupDirection::downwards);
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& e) {
  g.setColour(e.findColour(juce::TextEditor::backgroundColourId));
  g.fillRoundedRectangle(juce::Rectangle<float>((float)width, (float)height), kRadius);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& e) {
  if (!e.isEnabled()) return;
  const bool focus = e.hasKeyboardFocus(true) && !e.isReadOnly();
  g.setColour(e.findColour(focus ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
  g.drawRoundedRectangle(juce::Rectangle<float>((float)width, (float)height).reduced(0.5f), kRadius, focus ? 1.5f : 1.0f);
}

void LookAndFeel::drawMenuBarBackground(juce::Graphics& g, int width, int height, bool, juce::MenuBarComponent&) {
  g.fillAll(kBackground);  // the menus sit in the top bar, beside the logo
  juce::ignoreUnused(width, height);
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
  g.fillAll(kPanel);
  g.setColour(kOutline);
  g.drawRect(0, 0, width, height, 1);
}

// Menus and dropdown lists: rows of 26 px, a soft blue row under the mouse (not a solid block), a blue check for
// the chosen item, muted shortcuts and a small chevron for submenus.
void LookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                    bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                                    const juce::Drawable* icon, const juce::Colour* textColour) {
  if (isSeparator) {
    g.setColour(kOutline);
    g.fillRect(area.getX() + 8, area.getCentreY(), area.getWidth() - 16, 1);
    return;
  }
  auto r = area.reduced(4, 1);
  if (isHighlighted && isActive) {
    g.setColour(kBlueHeading);  // one step lighter in the family, the highlight only as the thin mark
    g.fillRoundedRectangle(r.toFloat(), kRadius);
    g.setColour(kAccent);
    g.fillRoundedRectangle(r.removeFromLeft(3).toFloat().reduced(0, 4), 1.5f);
  } else {
    r.removeFromLeft(3);
  }
  const juce::Colour colour = !isActive ? kDim : textColour != nullptr ? *textColour : kText;
  auto check = r.removeFromLeft(20).toFloat();
  if (isTicked) {
    juce::Path tick;
    const auto c = check.withSizeKeepingCentre(10, 8);
    tick.startNewSubPath(c.getX(), c.getCentreY());
    tick.lineTo(c.getX() + 3.5f, c.getBottom());
    tick.lineTo(c.getRight(), c.getY());
    g.setColour(isActive ? kAccentBright : kDim);
    g.strokePath(tick, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  } else if (icon != nullptr) {
    icon->drawWithin(g, check.reduced(3), juce::RectanglePlacement::centred, isActive ? 1.0f : 0.5f);
  }
  r.removeFromRight(8);
  if (hasSubMenu) {
    const auto c = r.removeFromRight(10).toFloat().withSizeKeepingCentre(5, 8);
    juce::Path chevron;
    chevron.startNewSubPath(c.getX(), c.getY());
    chevron.lineTo(c.getRight(), c.getCentreY());
    chevron.lineTo(c.getX(), c.getBottom());
    g.setColour(isHighlighted ? kText : kMuted);
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }
  g.setFont(getPopupMenuFont());
  if (shortcutKeyText.isNotEmpty()) {
    g.setColour(kMuted);
    g.drawText(shortcutKeyText, r, juce::Justification::centredRight, true);
    r.removeFromRight(juce::GlyphArrangement::getStringWidthInt(getPopupMenuFont(), shortcutKeyText) + 12);
  }
  g.setColour(colour);
  g.drawFittedText(lang::tr(text), r, juce::Justification::centredLeft, 1, 1.0f);
}

void LookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName) {
  g.setColour(kMuted);
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  g.drawText(lang::tr(sectionName).toUpperCase(), area.reduced(12, 0).withTrimmedTop(4), juce::Justification::centredLeft, true);
}

juce::Font LookAndFeel::getPopupMenuFont() { return juce::FontOptions(theme::kFontStrong); }
juce::Font LookAndFeel::getMenuBarFont(juce::MenuBarComponent&, int, const juce::String&) { return juce::FontOptions(theme::kFontStrong); }

void LookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                            int& idealHeight) {
  if (isSeparator) {
    idealWidth = 50;
    idealHeight = 9;
    return;
  }
  idealHeight = standardMenuItemHeight > 0 ? juce::jmax(standardMenuItemHeight, 24) : 26;
  idealWidth = juce::GlyphArrangement::getStringWidthInt(getPopupMenuFont(), lang::tr(text)) + 64;
}

// The top menus: muted names that turn white under the mouse, on a soft rounded field while open.
void LookAndFeel::drawMenuBarItem(juce::Graphics& g, int width, int height, int, const juce::String& itemText, bool isMouseOverItem,
                                  bool isMenuOpen, bool isMouseOverBar, juce::MenuBarComponent& bar) {
  const auto r = juce::Rectangle<float>((float)width, (float)height).reduced(1.0f, 2.0f);
  const bool lit = isMenuOpen || (isMouseOverItem && isMouseOverBar);
  if (lit) {
    g.setColour(isMenuOpen ? kControlHover : kControl);
    g.fillRoundedRectangle(r, kRadius);
  }
  g.setColour(lit ? kText : kText.withAlpha(0.82f));
  g.setFont(getMenuBarFont(bar, 0, itemText));
  g.drawText(lang::tr(itemText), r, juce::Justification::centred, false);
}

int LookAndFeel::getMenuBarItemWidth(juce::MenuBarComponent& bar, int itemIndex, const juce::String& itemText) {
  return juce::GlyphArrangement::getStringWidthInt(getMenuBarFont(bar, itemIndex, itemText), lang::tr(itemText)) + bar.getHeight();
}

void LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart,
                                int thumbSize, bool over, bool down) {
  if (thumbSize <= 0) return;
  auto thumb = vertical ? juce::Rectangle<int>(x, thumbStart, width, thumbSize) : juce::Rectangle<int>(thumbStart, y, thumbSize, height);
  // A neutral grey, lighter under the mouse (Diogo 2026-11-09: no blue bar under the mixer).
  g.setColour(over || down ? juce::Colour(0xff5d6875) : juce::Colour(0xff3a4450));
  g.fillRoundedRectangle(thumb.toFloat().reduced(2.0f), kCorner);
}

void LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) {
  const auto r = juce::Rectangle<float>((float)width, (float)height);
  g.setColour(kCard);
  g.fillRoundedRectangle(r, kRadius);
  g.setColour(kOutline);
  g.drawRoundedRectangle(r.reduced(0.5f), kRadius, 1.0f);
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody));
  g.drawFittedText(lang::tr(text), juce::Rectangle<int>(width, height).reduced(6, 3), juce::Justification::centredLeft, 6);
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) {
  return juce::LookAndFeel_V4::getTooltipBounds(lang::tr(tipText), screenPos, parentArea);
}

} // namespace core::app::theme
