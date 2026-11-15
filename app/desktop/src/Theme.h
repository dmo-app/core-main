// Core's look: one palette, the segmented-C logo and a LookAndFeel for every standard control.
// The colours are the Core Studio palette (Diogo's UI brief, 2026-11-08): charcoal surfaces, five steps of blue-grey
// (#B1B9CE, #8693B6, #5E6D96, #3D4B76, #343F5E) for structure and controls, and the lightest of them as the highlight for curves,
// fills, arcs and active controls that have no identity of their own. A track's own colour (TrackColour) wins over
// the highlight wherever something belongs to that track. Every window draws with these names, so a change of
// colour is made here only.
#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

namespace core::app::theme {

// The blue-grey family, lightest first.
inline const juce::Colour kBlueLight{0xffb1b9ce};    // titles and structural text
inline const juce::Colour kBlueSoft{0xff8693b6};     // outlines of chosen things, the hover of a chosen button
inline const juce::Colour kBlueBase{0xff5e6d96};     // controls that are on or pressed; the Master
inline const juce::Colour kBlueHeading{0xff3d4b76};  // card headers, the hover of a button
inline const juce::Colour kBlueDeep{0xff343f5e};     // deeper blue surfaces

// The square look (Diogo 2026-11-09, "normalizar o ui todo, mais este estilo quadrado"): one corner for every
// button, field, card, panel and window in Core. Tiny marks (a 1 px dot, a meter tick) may stay rounder.
inline constexpr float kCorner = 2.0f;

// UI sizes (CORE-MANUAL app/ui-sizes.md, next to ui-colours.md; Diogo 2026-11-09 "font muito pequeno, fazer uma lista de
// settings para o UI normalizar tudo"). Every text and spacing in Core comes from here, in logical pixels (View > UI
// size scales them all). Five text sizes, nothing in between:
inline constexpr float kFontSmall = 13.0f;    // values, rulers, hints, small print
inline constexpr float kFontBody = 14.5f;     // labels, list rows, buttons, fields
inline constexpr float kFontStrong = 15.5f;   // names, card and section titles (often bold)
inline constexpr float kFontTitle = 17.0f;    // panel and window titles
inline constexpr float kFontHeading = 19.0f;  // the biggest: page headings, the clock
// Spacing: the gap between neighbours, the padding inside a box, and the heights of a row, a button and a header.
inline constexpr int kSpaceGap = 8, kSpacePad = 12, kSpaceRow = 30, kSpaceButton = 28, kSpaceHeader = 32;

// Surfaces, darkest first (charcoal and greys are structure only).
inline const juce::Colour kBackground{0xff10151b};  // the window
inline const juce::Colour kPanel{0xff171f28};       // panels: tracks, editor, effects
inline const juce::Colour kCard{0xff1b242e};        // cards inside a panel: track headers, effect cards
inline const juce::Colour kField{0xff111922};       // text fields, value bars, lanes, plots
inline const juce::Colour kGrid{0xff222c38};        // grid lines on a lane or a plot
inline const juce::Colour kOutline{0xff303c4b};     // borders and separators

// Controls (buttons): normal, then hover one step lighter in the same family (no glow, no shadow).
inline const juce::Colour kControl{0xff1f2934};
inline const juce::Colour kControlHover{kBlueHeading};
// Every button is a solid block, never an empty box: dark (this) when off, light (kBlueLight) under the mouse, while
// pressed and when on or chosen. No outline. (Diogo 2026-11-09: "não quero botões vazios… o hover, selecionar e click
// mais claro, isso já devia ser regra geral".)
inline const juce::Colour kButtonDark{kBlueDeep};

// Text.
inline const juce::Colour kText{0xffe1e6ef};
inline const juce::Colour kMuted{0xff929fb1};
inline const juce::Colour kDim{0xff5f6b7d};

// The main colour: buttons that are on, the chosen tab, focus, menu rows.
inline const juce::Colour kPrimary{kBlueBase};
inline const juce::Colour kPrimaryLight{kBlueSoft};  // focus rings, the hover of a chosen button
inline const juce::Colour kTitle{kBlueLight};        // section titles and highlights in text
// The highlight: play, meters, waveforms, notes, curves of ENV/LFO, the selection, the loop, and knob arcs and value
// fills that have no track of their own. No blue (Diogo 2026-11-09: "não quero efeito azul no selecionado, usar sempre
// os mesmos tons mais claros ou escuros"): it is the light blue-grey of the family, and one step lighter under the mouse.
// What belongs to a track takes the track's colour instead (theme::identityOf).
inline const juce::Colour kAccent{kBlueLight};
inline const juce::Colour kAccentBright{0xffd6dbe7};  // the same, one step lighter (hover, drag)
// The start line (the playhead: where Play starts and what is heard): its own warm colour, the only one on the
// timeline, so it is never taken for the loop or a selection (Diogo 2026-11-09: "não gosto da cor do iniciar").
inline const juce::Colour kStartLine{0xffddb07c};

// Clips on the arrangement: a deep blue body with its notes drawn lighter.
inline const juce::Colour kClip{kBlueDeep};
inline const juce::Colour kClipNote{kBlueLight};

// States (soft, never loud).
inline const juce::Colour kOk{0xff3ccf6e};     // states only: saturated, never a track's pastel (ui-colours.md)
inline const juce::Colour kWarn{0xffffb020};
inline const juce::Colour kError{0xffe0504a};

// A TextButton with this property ("home", "play", "pause", "stop", "loop") is drawn as that icon instead of its text.
inline const juce::Identifier kIconProperty{"coreIcon"};

// Small line icons in the current colour: play, stop, home, loop, pencil, select, erase, folder, star,
// starOn, wave, down, right, search, library, inspector.
void drawIcon(juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area);

// The segmented C (three pieces and the diamond at its core), scaled to fit the area.
juce::Path logoPath(juce::Rectangle<float> area);
void drawLogo(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour = kText);
// The logo and "CORE" beside it, as on the top bar.
void drawWordmark(juce::Graphics& g, juce::Rectangle<float> area);

// Identity (Diogo 2026-11-08): knobs and value bars take the colour of the track or group they belong to, not the
// generic highlight. A panel that edits one track sets its colour once on itself; every knob and bar inside it, at
// any depth, draws its arc or fill in that colour. Nothing set anywhere up the parents: the highlight (light blue-grey).
inline const juce::Identifier kIdentityProperty{"coreIdentity"};
// A level (a track's volume, a fader, a meter): drawn in the light blue-grey, not in a track's colour (Diogo 2026-11-09:
// "o azul ... pode ser azul cinza claro", "nos níveis"). Set with markLevel.
inline const juce::Identifier kLevelProperty{"coreLevel"};
inline void markLevel(juce::Component& c) { c.getProperties().set(kLevelProperty, true); }
// A value field (the tempo): a plain dark field with its number big and centred, no fill behind it (Diogo 2026-11-09:
// "bpm mal se vê").
inline const juce::Identifier kPlainProperty{"corePlain"};
inline void markPlain(juce::Component& c) { c.getProperties().set(kPlainProperty, true); }
void setIdentity(juce::Component& c, juce::Colour colour);  // transparent clears it
juce::Colour identityOf(const juce::Component& c, juce::Colour fallback = kAccent);

// A card (an effect, a module of an editor): a blue header band and a charcoal body, fine outline, 6 px corners
// (brief 2026-11-08: the identity colour goes on the header, never over the whole body). With an identity colour
// (the track's, a layer's) the band is tinted by it and carries a 3 px edge of it on top, as the Mixer strips do;
// without one the band is the deep blue of the palette. alpha < 1 draws a card that is off (bypassed).
// selected: a 2 px line all round in the identity colour; cards that are not selected keep the neutral outline.
void paintCard(juce::Graphics& g, juce::Rectangle<float> r, float headerH, juce::Colour identity = {}, float alpha = 1.0f,
               bool selected = false);
// The title over a plot or a group (Spectrum, Pitch Envelope, LAYERS · MIX): a 3 x 12 px mark in the identity
// colour (the highlight by default) and the words in the light blue-grey, semibold.
void paintSectionTitle(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, juce::Colour mark = kAccent);

// Rule 11 (Diogo 2026-11-09, CORE-MANUAL app/ui-colours.md): a panel with several sections gives each one a tone of
// a scale, light at the top and darker on each section below. A track's panel takes the scale from the track's
// colour; a neutral panel (no track) the blue-grey palette, kBlueLight down to kBlueHeading. index 0 = the top one.
juce::Colour toneScale(juce::Colour base, int count, int index);
juce::Colour neutralScale(int count, int index);
// A section in its tone (as the Kick Designer and the Core FX): the header tinted ~35 %, a 2 px border in the
// tone, the body charcoal; the title in capitals in a light tone of the same colour.
void paintToneSection(juce::Graphics& g, juce::Rectangle<float> r, float headerH, juce::Colour tone, const juce::String& title = {},
                      float alpha = 1.0f);

class LookAndFeel final : public juce::LookAndFeel_V4 {
public:
  LookAndFeel();

  void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background, bool over, bool down) override;
  void drawButtonText(juce::Graphics&, juce::TextButton&, bool over, bool down) override;
  juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
  int getTextButtonWidthToFitText(juce::TextButton&, int buttonHeight) override;
  // Labels and tick boxes say their words in Core's language (Language.h); a label you can edit (a name you
  // typed) is never translated. drawLabel (below) draws a slider's value box and every other label through this.
  void drawPlainLabel(juce::Graphics&, juce::Label&);
  void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
  void changeToggleButtonWidthToFitText(juce::ToggleButton&) override;
  std::unique_ptr<juce::AlertWindow> createAlertWindowForOptions(const juce::MessageBoxOptions&) override;

  // Dialogs in the app's style: a card, and a small round mark (?, !, i) instead of JUCE's large icons.
  void drawAlertBox(juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
  void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float pos, float minPos, float maxPos,
                        juce::Slider::SliderStyle, juce::Slider&) override;
  void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float pos, float startAngle, float endAngle,
                        juce::Slider&) override;
  juce::Font getLabelFont(juce::Label&) override;
  void drawLabel(juce::Graphics&, juce::Label&) override;
  juce::Button* createSliderButton(juce::Slider&, bool isIncrement) override;
  juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;

  void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY, int buttonW, int buttonH,
                    juce::ComboBox&) override;
  juce::Font getComboBoxFont(juce::ComboBox&) override;
  juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox&, juce::Label&) override;

  void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
  void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

  void drawMenuBarBackground(juce::Graphics&, int width, int height, bool isMouseOverBar, juce::MenuBarComponent&) override;
  void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
  void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                         bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                         const juce::Drawable* icon, const juce::Colour* textColour) override;
  void drawPopupMenuSectionHeader(juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
  juce::Font getPopupMenuFont() override;
  juce::Font getMenuBarFont(juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;
  void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                 int& idealHeight) override;
  void drawMenuBarItem(juce::Graphics&, int width, int height, int itemIndex, const juce::String& itemText, bool isMouseOverItem,
                       bool isMenuOpen, bool isMouseOverBar, juce::MenuBarComponent&) override;
  int getMenuBarItemWidth(juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;

  void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart,
                     int thumbSize, bool over, bool down) override;

  void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;
  juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
};

// A value bar (Slider::LinearBar: the track volume, the tempo, a send, an effect's values) that you drag
// (Diogo 2026-11-07): one click and a drag change the value, never a text box; a double-click opens the box to
// type a value; Alt+click goes back to the default (setDoubleClickReturnValue). A plain juce::Slider opens the
// text box on a single click, and a second click then lands in the text instead of dragging.
class BarSlider final : public juce::Slider {
public:
  BarSlider() : juce::Slider(juce::Slider::LinearBar, juce::Slider::TextBoxLeft) { typeOnDoubleClick(); }

  // The double-click types (the box itself opens its editor), it does not reset: Alt+click resets.
  void mouseDoubleClick(const juce::MouseEvent&) override {}
  // JUCE makes the box again (style, look) or sets it editable on a single click (enabled again): undo that each time.
  void lookAndFeelChanged() override { juce::Slider::lookAndFeelChanged(); typeOnDoubleClick(); }
  void enablementChanged() override { juce::Slider::enablementChanged(); typeOnDoubleClick(); }

private:
  void typeOnDoubleClick() {
    for (auto* c : getChildren())
      if (auto* box = dynamic_cast<juce::Label*>(c); box && box->isEditable()) box->setEditable(false, true);
  }
};

// The explanation of anything (rule for all of Core, Diogo 2026-11-05): never a sentence on show, but a small
// grey ball, no letter or icon, that turns blue on hover and shows the words as
// a tip. Amber at rest for a warning.
class InfoDot final : public juce::Component, public juce::SettableTooltipClient {
public:
  static constexpr float kSize = 6.0f;
  static constexpr int kSlot = 12;  // its hit box next to its word: kSlot x kSlot
  explicit InfoDot(bool warn = false) : warn_(warn) { setWantsKeyboardFocus(false); }
  void setWarn(bool warn) { warn_ = warn; repaint(); }
  void paint(juce::Graphics& g) override {
    // Minuscule (Diogo): a 6 px dot right next to its word, slider or button; the hover area is the whole component.
    const auto circle = juce::Rectangle<float>(kSize, kSize).withCentre(getLocalBounds().toFloat().getCentre());
    g.setColour(isMouseOver() ? kAccent : warn_ ? kWarn : kDim);
    g.fillEllipse(circle);
  }
  void mouseEnter(const juce::MouseEvent&) override { repaint(); }
  void mouseExit(const juce::MouseEvent&) override { repaint(); }

private:
  bool warn_ = false;
};

// On / bypass (the brief, 2026-11-09): a small circle in a lighter tone of the owner's colour, full when on, only its
// outline when off. A TextButton underneath (toggle state = on, its name kept for accessibility), so it drops in for
// an "On" button.
class OnCircle final : public juce::TextButton {
public:
  OnCircle() : juce::TextButton("On") { setWantsKeyboardFocus(false); setMouseCursor(juce::MouseCursor::PointingHandCursor); }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const juce::Colour light = identityOf(*this, kBlueSoft).interpolatedWith(juce::Colours::white, down ? 0.15f : over ? 0.5f : 0.35f);
    const auto c = juce::Rectangle<float>(13.0f, 13.0f).withCentre(getLocalBounds().toFloat().getCentre());
    g.setColour(light);
    if (getToggleState()) g.fillEllipse(c);
    g.drawEllipse(c.reduced(0.75f), 1.5f);
  }
};

// The plugins' controls are blue value bars, not knobs (Diogo 2026-11-07: "trocar knobs por sliders azuis, porque
// são mais fáceis de mexer com o rato"): drag or click along the bar; the value is written inside it.
inline void useBar(juce::Slider& s) {
  s.setSliderStyle(juce::Slider::LinearBar);
  s.setTextBoxStyle(juce::Slider::TextBoxLeft, true, 80, 20);
  s.setColour(juce::Slider::textBoxTextColourId, kText);
  s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
}
constexpr int kBarH = 22;  // a value bar's height in the plugin cards

} // namespace core::app::theme
