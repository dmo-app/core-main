#include "BuildupPanel.h"
#include "Language.h"
#include <algorithm>
#include <map>
#include "Theme.h"

namespace core::app {
using namespace theme;

namespace {
juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
juce::String pitchName(int p) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return juce::String(names[p % 12]) + juce::String(p / 12 - 1);
}
constexpr int64_t kBar = 4 * doc::kPpq;
constexpr int kRowH = 26, kGap = 6, kLabelW = 104, kPad = 14, kWidth = 420, kHeadH = 26, kTopH = 46;
constexpr int kNone = 100000;  // "nothing" / "keep the notes there" in a box of the library
// The pattern menus, one submenu per style (Diogo, 2026-11-05: "mostra tudo, apenas organiza").
const char* kStyles[] = {"darkpsycore", "hitech", "psytrance", "slambient"};
const char* kStyleNames[] = {"Dark psycore", "Hi-tech", "Psytrance", "Slambient"};

void bars(juce::Slider& s, int value) {
  s.setSliderStyle(juce::Slider::IncDecButtons);
  s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 40, kRowH - 4);
  s.setRange(1, 64, 1);
  s.setValue(value, juce::dontSendNotification);
}
void amount(juce::Slider& s, double lo, double hi, double value, const juce::String& suffix, const juce::String& tip) {
  s.setSliderStyle(juce::Slider::LinearHorizontal);  // a thin line and a thumb, the value at the right (ref-2)
  s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, kRowH - 4);
  s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  s.setColour(juce::Slider::textBoxTextColourId, kText);
  s.setRange(lo, hi, 1);
  s.setValue(value, juce::dontSendNotification);
  s.setDoubleClickReturnValue(true, value);
  s.setTextValueSuffix(suffix);
  s.setTooltip(tip);
}
// A style's patterns, in sections by role; item ids are 1 + the index in the library.
juce::PopupMenu styleMenu(const std::vector<doc::PatternPreset>& lib, const std::string& style, const std::string& chosen) {
  std::map<std::string, std::vector<int>> byRole;
  for (size_t i = 0; i < lib.size(); ++i)
    if (style.empty() || std::find(lib[i].styles.begin(), lib[i].styles.end(), style) != lib[i].styles.end())
      byRole[lib[i].role.empty() ? std::string("other") : lib[i].role].push_back((int)i);
  juce::PopupMenu m;
  for (const auto& entry : byRole) {
    const juce::String role = u8(entry.first);
    m.addSectionHeader(role.substring(0, 1).toUpperCase() + role.substring(1));
    for (int i : entry.second)
      m.addItem(juce::PopupMenu::Item(u8(lib[(size_t)i].name)).setID(i + 1).setTicked(lib[(size_t)i].id == chosen));
  }
  return m;
}
juce::String nameOf(const std::vector<doc::PatternPreset>& lib, const std::string& id, const char* none) {
  for (const auto& p : lib) if (p.id == id) return u8(p.name) + u8("  ▾");
  return u8(none) + u8("  ▾");
}
// A section's head is drawn by the panel (its title in small capitals, a chevron at the right, as FAVORITES and
// SOUNDS in the Library); the button is only the place to click, with no box or text of its own.
void head(juce::TextButton& b, const juce::String&, bool open) {
  b.setButtonText({});
  b.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
  b.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
  b.setToggleState(open, juce::dontSendNotification);
  b.setMouseCursor(juce::MouseCursor::PointingHandCursor);
}
}  // namespace

BuildupPanel::BuildupPanel(const Start& start) : start_(start) {
  status_.setColour(juce::Label::textColourId, kMuted);
  status_.setFont(juce::FontOptions(theme::kFontBody));
  status_.setJustificationType(juce::Justification::topLeft);
  status_.setText(u8("Writes notes once. Again: another take in its place. Ctrl+Z undoes."), juce::dontSendNotification);
  addAndMakeVisible(status_);

  // --- always shown
  pattern_.setTooltip(u8("A pattern of the library for these bars (by style; any one fits any track), or keep the notes there"));
  pattern_.onClick = [this] { choose(false); };
  drop_.setTooltip(u8("A pattern written after the drop"));
  drop_.onClick = [this] { choose(true); };
  for (juce::TextButton* b : {&pattern_, &drop_}) {  // a field that opens a menu, as the combo boxes
    b->setColour(juce::TextButton::buttonColourId, kField);
    b->setColour(juce::TextButton::textColourOffId, kText);
  }
  showChoices();
  speed_.addItem(u8("½×  half time"), 1);
  speed_.addItem(u8("1×  as written"), 2);
  speed_.addItem(u8("2×  double time"), 3);
  speed_.setSelectedId(2, juce::dontSendNotification);
  speed_.setTooltip(u8("Half time keeps a psytrance part's feel in a song twice as fast (slambient); double time the other way"));
  bars(from_, start_.fromBar);
  bars(to_, start_.toBar);
  dash_.setText(u8("to"), juce::dontSendNotification);
  dash_.setJustificationType(juce::Justification::centred);

  // --- Change
  amount(density_, -90, 200, 0, " %", u8("More notes (fills of the empty 1/16) or fewer (the ones on the beat stay)"));
  amount(chaos_, 0, 100, 0, " %", u8("This share of the notes gets a random note between the two notes"));
  const int base = start_.key >= 0 ? start_.key / 12 * 12 : 36;
  for (int p = 0; p < 128; ++p) { low_.addItem(pitchName(p), p + 1); high_.addItem(pitchName(p), p + 1); }
  low_.setSelectedId(base + 1, juce::dontSendNotification);
  high_.setSelectedId(base + 7, juce::dontSendNotification);  // C to F#
  between_.setText("-", juce::dontSendNotification);
  between_.setJustificationType(juce::Justification::centred);
  amount(accent_, 0, 100, 0, " %", u8("How much the accents shape the velocity"));
  accentShape_.addItem(u8("KBBB"), 1);
  accentShape_.addItem(u8("Rising"), 2);
  accentShape_.setSelectedId(1, juce::dontSendNotification);
  accentShape_.setTooltip(u8("KBBB: the note after the kick loudest (1 · .72 · .52 · .64). Rising: every beat gets louder to its end (.62 · .74 · .87 · 1)"));
  amount(gate_, 0, 100, 0, " %", u8("0: the lengths stay; else every note lasts this share of a 1/16"));

  // --- Build-up -> roll -> drop
  roll_.addItem(u8("Gradual (speeds up to the drop)"), 1);
  roll_.addItem(u8("Even 1/32"), 2);
  roll_.setSelectedId(1, juce::dontSendNotification);
  bars(buildAt_, start_.fromBar);
  buildAt_.setTooltip(u8("The bar the build-up starts (1/4, 1/8, 1/16). The same as the roll: no build-up"));
  bars(rollAt_, start_.fromBar);
  rollAt_.setTooltip(u8("The bar the roll starts. The drop is the bar after the last one"));
  rollWord_.setText(u8("roll at"), juce::dontSendNotification);
  rollWord_.setJustificationType(juce::Justification::centred);
  for (int b = 0; b <= 4; ++b) gap_.addItem(b == 0 ? u8("None") : juce::String(b) + (b == 1 ? " beat" : " beats"), b + 1);
  gap_.setSelectedId(1, juce::dontSendNotification);
  for (int st : {0, 3, 5, 7, 12}) rise_.addItem(st == 0 ? u8("None") : "+" + juce::String(st) + " st", st + 1);
  rise_.setSelectedId(1, juce::dontSendNotification);
  rise_.setTooltip(u8("The roll's notes climb this far by the drop (a 909 snare roll). On a sampler or synth track; the built-in drums keep their sound"));
  for (int b : {1, 2, 4, 8}) dropBars_.addItem(juce::String(b) + (b == 1 ? " bar" : " bars"), b);
  dropBars_.setSelectedId(2, juce::dontSendNotification);
  tempoOn_.setButtonText(u8("Glide to"));
  if (!start_.tempoPossible) tempoOn_.setTooltip(u8("The pattern is not in the song yet: place a clip of it first"));
  tempoOn_.setEnabled(start_.tempoPossible);
  amount(tempo_, 40, 600, std::round(start_.bpm), " BPM", u8("The song's tempo glides to this from the build-up (or roll) to the drop"));
  tempoOn_.onClick = [this] { tempo_.setEnabled(tempoOn_.getToggleState()); };
  effects_.addItem(u8("None"), 1);
  effects_.addItem(u8("Filter opens + reverb grows"), 2);
  effects_.addItem(u8("Filter + reverb + delay"), 3);
  effects_.addItem(u8("Filter + reverb + delay + drive"), 4);
  effects_.setSelectedId(1, juce::dontSendNotification);
  effects_.setTooltip(u8("The plugins of the roles (Settings > Roles) on this track, moving from the build-up (or roll) to the drop"));
  tempo_.setEnabled(false);

  head(changeHead_, u8("Change"), false);
  head(dropHead_, u8("Build-up → roll → drop"), false);
  changeHead_.setTooltip(u8("More or fewer notes, random notes, accents, shorter notes (closed: nothing changes)"));
  dropHead_.setTooltip(u8("A transition into the drop: build-up, roll, silence, what comes after, the tempo (closed: none)"));
  changeHead_.onClick = [this] { changeOpen_ = !changeOpen_; relayout(); };
  changeHead_.onStateChange = [this] { repaint(); };  // the title lights under the mouse
  dropHead_.onStateChange = [this] { repaint(); };
  dropHead_.onClick = [this] { dropOpen_ = !dropOpen_; relayout(); };
  write_.setButtonText("Write");
  write_.setColour(juce::TextButton::buttonColourId, kPrimary);
  again_.setButtonText("Again");
  again_.setTooltip(u8("The last take goes, another one is written (new random draws)"));
  again_.setEnabled(false);
  write_.onClick = [this] { write(false); };
  again_.onClick = [this] { write(true); };

  top_ = {{"Pattern", {&pattern_}, {0}},
          {"Speed", {&speed_}, {0}},
          {"Bars", {&from_, &dash_, &to_}, {110, 30, 110}}};
  change_ = {{"More / fewer", {&density_}, {0}},
             {"Random notes", {&chaos_, &low_, &between_, &high_}, {0, 62, 14, 62}},
             {"Accents", {&accent_, &accentShape_}, {0, 86}},
             {"Shorter notes", {&gate_}, {0}}};
  drop_rows_ = {{"Roll", {&roll_}, {0}},
                {"Build-up at", {&buildAt_, &rollWord_, &rollAt_}, {110, 56, 110}},
                {"Silence", {&gap_}, {0}},
                {"Pitch rise", {&rise_}, {0}},
                {"After the drop", {&drop_, &dropBars_}, {0, 80}},
                {"Tempo", {&tempoOn_, &tempo_}, {90, 0}},
                {"Effects", {&effects_}, {0}}};
  for (auto* rows : {&top_, &change_, &drop_rows_})
    for (auto& row : *rows) {
      auto* l = labels_.add(new juce::Label({}, row.label));
      l->setColour(juce::Label::textColourId, kMuted);
      l->setFont(juce::FontOptions(theme::kFontBody));
      addChildComponent(l);
      for (auto* c : row.parts) addChildComponent(c);
    }
  for (juce::Component* c : {(juce::Component*)&changeHead_, (juce::Component*)&dropHead_, (juce::Component*)&write_, (juce::Component*)&again_})
    addAndMakeVisible(c);
  relayout();
}

void BuildupPanel::showChoices() {
  pattern_.setButtonText(nameOf(start_.library, templateId_, "Keep the notes there"));
  drop_.setButtonText(nameOf(start_.library, dropId_, "Nothing"));
}

void BuildupPanel::choose(bool drop) {
  const std::string& chosen = drop ? dropId_ : templateId_;
  juce::PopupMenu m;
  m.addItem(juce::PopupMenu::Item(drop ? u8("Nothing") : u8("Keep the notes there (change them below)")).setID(kNone).setTicked(chosen.empty()));
  m.addSeparator();
  for (int i = 0; i < 4; ++i) m.addSubMenu(u8(kStyleNames[i]), styleMenu(start_.library, kStyles[i], chosen));
  m.addSubMenu(u8("All patterns"), styleMenu(start_.library, "", chosen));
  m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(drop ? &drop_ : &pattern_),
                  [safe = juce::Component::SafePointer<BuildupPanel>(this), drop](int r) {
    if (!safe || r <= 0) return;
    const std::string id = r == kNone || r > (int)safe->start_.library.size() ? std::string() : safe->start_.library[(size_t)r - 1].id;
    (drop ? safe->dropId_ : safe->templateId_) = id;
    safe->showChoices();
  });
}

doc::PCoreSettings BuildupPanel::settings() const {
  doc::PCoreSettings s;
  s.mode = templateId_.empty() ? "alter" : "create";
  s.templateId = templateId_;
  s.speed = speed_.getSelectedId() == 1 ? 0.5 : speed_.getSelectedId() == 3 ? 2.0 : 1.0;
  if (changeOpen_) {
    s.density = density_.getValue();
    s.accent = accent_.getValue() / 100.0;
    s.accentShape = accentShape_.getSelectedId() == 2 ? "rising" : "kbbb";
    s.gate = gate_.getValue() / 100.0;
    s.pitchChance = chaos_.getValue() / 100.0;
    if (s.pitchChance > 0) {
      s.pitchLow = std::min(low_.getSelectedId(), high_.getSelectedId()) - 1;
      s.pitchHigh = std::max(low_.getSelectedId(), high_.getSelectedId()) - 1;
    }
  }
  if (dropOpen_) {  // an open section is a transition
    s.roll = roll_.getSelectedId() == 1 ? "gradual" : "steps";
    const int b = (int)buildAt_.getValue(), r = std::max(b, (int)rollAt_.getValue());
    s.rollStart = (int64_t)(r - 1) * kBar;
    s.buildupStart = b < r ? (int64_t)(b - 1) * kBar : -1;
    s.gapBeats = gap_.getSelectedId() - 1;
    s.rollRise = rise_.getSelectedId() - 1;
    if (!dropId_.empty()) {
      s.dropTemplate = dropId_;
      s.dropBars = dropBars_.getSelectedId();
    }
  }
  return s;
}

void BuildupPanel::write(bool again) {
  if (!onWrite) return;
  const int from = (int)from_.getValue(), to = (int)to_.getValue();
  if (to < from) { status_.setText(u8("The last bar comes after the first."), juce::dontSendNotification); return; }
  if (dropOpen_ && (buildAt_.getValue() < from || rollAt_.getValue() > to)) {
    status_.setText(u8("The build-up and the roll start inside bars ") + juce::String(from) + u8("–") + juce::String(to) + ".",
                    juce::dontSendNotification);
    return;
  }
  const double tempoTo = dropOpen_ && tempoOn_.isEnabled() && tempoOn_.getToggleState() ? tempo_.getValue() : 0.0;
  const int fx = dropOpen_ ? effects_.getSelectedId() - 1 : 0;
  const juce::String said = onWrite(settings(), (int64_t)(from - 1) * kBar, (int64_t)to * kBar, tempoTo, fx, again && written_);
  status_.setText(said, juce::dontSendNotification);
  written_ = true;
  again_.setEnabled(true);
}

void BuildupPanel::paint(juce::Graphics& g) {
  g.fillAll(kPanel);
  const int inner = getWidth() - 2 * kPad;
  // Header as the Inspector's: an icon, the name in capitals, a line under it.
  auto top = juce::Rectangle<int>(kPad, kPad, inner, kTopH - kPad - 8);
  drawLogo(g, top.removeFromLeft(18).toFloat().reduced(0, 2), kAccent);
  top.removeFromLeft(10);
  g.setColour(kText);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
  g.drawText(lang::tr("BUILD-UP"), top.removeFromLeft(80), juce::Justification::centredLeft);
  g.setColour(kDim);
  g.setFont(juce::FontOptions(theme::kFontBody));
  g.drawText(lang::tr(u8("roll → drop · writes once")), top, juce::Justification::centredRight);
  g.setColour(kOutline);
  g.fillRect(kPad, kTopH - 4, inner, 1);
  // The sections: a line over each, the title in small capitals, what a closed one holds, the chevron.
  auto section = [&](const juce::TextButton& b, const juce::String& title, const char* closedHint, bool open) {
    const auto r = b.getBounds();
    g.setColour(kOutline);
    g.fillRect(kPad, r.getY() - 6, inner, 1);
    g.setColour(open || b.isMouseOver() ? kTitle : kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold).withKerningFactor(0.06f));
    auto t = r;
    g.drawText(title, t.removeFromLeft(t.getWidth() / 2 + 40), juce::Justification::centredLeft);
    drawIcon(g, open ? "down" : "right", t.removeFromRight(16).toFloat());
    if (!open) {
      g.setColour(kDim);
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.drawText(u8(closedHint), t.withTrimmedRight(6), juce::Justification::centredRight);
    }
  };
  section(changeHead_, "CHANGE", "off", changeOpen_);
  section(dropHead_, u8("BUILD-UP → ROLL → DROP"), "off", dropOpen_);
}

void BuildupPanel::relayout() {
  head(changeHead_, u8("Change"), changeOpen_);
  head(dropHead_, u8("Build-up → roll → drop"), dropOpen_);
  int i = 0;
  auto show = [&](std::vector<Row>& rows, bool on) {
    for (auto& row : rows) {
      labels_[i++]->setVisible(on);
      for (auto* c : row.parts) c->setVisible(on);
    }
  };
  show(top_, true);
  show(change_, changeOpen_);
  show(drop_rows_, dropOpen_);
  setSize(kWidth, layout(false));  // the call-out box follows the new size
  resized();
  repaint();
}

int BuildupPanel::layout(bool place) {
  const int inner = kWidth - 2 * kPad;
  int y = kTopH + 8, labelIndex = 0;
  auto at = [&](juce::Component& c, juce::Rectangle<int> r) { if (place) c.setBounds(r); };
  auto rows = [&](std::vector<Row>& list, bool on) {
    for (auto& row : list) {
      juce::Label* l = labels_[labelIndex++];
      if (!on) continue;
      juce::Rectangle<int> r(kPad, y, inner, kRowH);
      at(*l, r.removeFromLeft(kLabelW));
      int fixed = 0;
      for (int w : row.widths) fixed += w;
      const int rest = std::max(40, r.getWidth() - fixed);
      for (size_t k = 0; k < row.parts.size(); ++k) at(*row.parts[k], r.removeFromLeft(row.widths[k] > 0 ? row.widths[k] : rest));
      y += kRowH + kGap;
    }
  };
  rows(top_, true);  // the pattern, the speed and the bars
  auto section = [&](juce::TextButton& headButton, std::vector<Row>& list, bool open) {
    y += 12;  // the line is drawn 6 px over the head
    at(headButton, {kPad, y, inner, kHeadH - 6});
    y += kHeadH;
    rows(list, open);
  };
  section(changeHead_, change_, changeOpen_);
  section(dropHead_, drop_rows_, dropOpen_);
  y += 14;
  // As the Inspector's Apply: the main action across the panel in the accent, Again beside it.
  at(write_, {kPad, y, inner - 104, 32});
  at(again_, {kPad + inner - 96, y, 96, 32});
  y += 40;
  at(status_, {kPad, y, inner, 34});
  return y + 34 + kPad;
}

void BuildupPanel::resized() { layout(true); }

} // namespace core::app
