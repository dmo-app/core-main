#include "SCorePanel.h"
#include <algorithm>
#include "edit.hpp"
#include "score.hpp"

namespace core::app {

namespace {
juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }
using theme::kPanel, theme::kCard, theme::kText, theme::kMuted, theme::kAccent, theme::kField, theme::kGrid;
constexpr int kHeader = 34, kGap = 10, kNameW = 190, kHead = 26, kRowH = 24;
constexpr int kDot = theme::InfoDot::kSlot;
}  // namespace

SCorePanel::SCorePanel(PatternEditor::Host& host) : host_(host) {
  title_.setText(u8("Stage Core (S-Core)"), juce::dontSendNotification);
  title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, kText);
  titleDot_.setTooltip(u8("Stage Core builds the song's parts: which tracks play in the intro, the build-up, the drop, the "
                          "break... Click a square to turn a track on or off in a section. Click a track's name to open "
                          "its own plugin (Bass, Synth) or its notes."));
  shape_.setButtonText(u8("Song shape  ▾"));
  shape_.onClick = [this] { showShapes(); };
  shapeDot_.setTooltip(u8("Lays out the sections for a style (intro, build-up, drop, break, roll, outro, with their "
                          "lengths). The clips stay where they are; Arrange then places them."));
  arrange_.setButtonText(u8("Arrange"));
  arrange_.onClick = [this] { arrange(); };
  arrangeDot_.setTooltip(u8("Puts the selected tracks (or every track, when only one is selected) where their part plays: "
                            "no kick in the break, hats later in the drop, the lead in the second half, pads in the intro "
                            "and the break, no kick and bass in the bar before each drop. One Ctrl+Z."));
  rise_.setButtonText(u8("Rise on build-ups"));
  rise_.onClick = [this] { rises(); };
  riseDot_.setTooltip(u8("Puts FX Core's build-up (the rising curve) over every build-up section, on the tracks that have "
                         "FX Core in region mode (a riser, a noise sweep)."));
  close_.setButtonText(u8("✕"));
  close_.onClick = [this] { if (onClose) onClose(); };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&title_, &titleDot_, &shape_, &shapeDot_, &arrange_, &arrangeDot_,
                                                                    &rise_, &riseDot_, &close_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
}

std::vector<std::string> SCorePanel::rows() const {
  std::vector<std::string> out;
  for (const auto& t : host_.currentDocument().tracks)
    if (doc::playsPatterns(t)) out.push_back(t.id);
  return out;
}

juce::Rectangle<int> SCorePanel::grid() const {
  return getLocalBounds().reduced(kGap, 0).withTrimmedTop(kHeader + 4).withTrimmedBottom(kGap);
}

SCorePanel::Cell SCorePanel::cellAt(juce::Point<int> p) const {
  const auto g = grid();
  const auto& d = host_.currentDocument();
  const auto ids = rows();
  if (!g.contains(p) || d.sections.empty()) return {};
  const int row = (p.y - g.getY() - kHead) / kRowH;
  if (p.y < g.getY() + kHead || row >= (int)ids.size()) return {};
  if (p.x < g.getX() + kNameW) return {row, -1};
  const int64_t total = std::max<int64_t>(1, d.sections.back().startTick + d.sections.back().lengthTicks);
  const double x = (double)(p.x - g.getX() - kNameW) / std::max(1, g.getWidth() - kNameW) * (double)total;
  for (size_t i = 0; i < d.sections.size(); ++i)
    if (x >= (double)d.sections[i].startTick && x < (double)(d.sections[i].startTick + d.sections[i].lengthTicks)) return {row, (int)i};
  return {row, -2};
}

void SCorePanel::documentChanged() {
  const bool any = !host_.currentDocument().sections.empty();
  arrange_.setEnabled(any);
  rise_.setEnabled(any);
  repaint();
}

void SCorePanel::paint(juce::Graphics& g) {
  g.fillAll(kPanel);
  const auto r = grid();
  g.setColour(kCard);
  g.fillRoundedRectangle(r.toFloat(), theme::kCorner);
  const auto& d = host_.currentDocument();
  const auto ids = rows();
  if (d.sections.empty()) {
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(u8("No sections yet: pick a Song shape above (or add sections on the ruler)."), r, juce::Justification::centred);
    return;
  }
  const int64_t total = std::max<int64_t>(1, d.sections.back().startTick + d.sections.back().lengthTicks);
  const int x0 = r.getX() + kNameW, w = r.getWidth() - kNameW;
  auto xAt = [&](int64_t tick) { return x0 + (int)((double)tick / (double)total * w); };
  // Section heads, each in its colour.
  for (const auto& s : d.sections) {
    const int a = xAt(s.startTick), b = xAt(s.startTick + s.lengthTicks);
    const juce::Colour c = juce::Colour::fromString("ff" + juce::String(doc::sectionColor(s)).substring(1));
    g.setColour(c.withAlpha(0.35f));
    g.fillRect(juce::Rectangle<int>(a + 1, r.getY() + 3, b - a - 2, kHead - 6));
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    g.drawFittedText(u8(s.name) + "  " + juce::String((int)(s.lengthTicks / (4 * doc::kPpq))), a + 4, r.getY() + 3, b - a - 8, kHead - 6,
                     juce::Justification::centredLeft, 1);
  }
  // One row per note track: its name and part, then a bar where it plays (its clips), dimmed where it rests.
  for (size_t i = 0; i < ids.size(); ++i) {
    const doc::Track* t = d.findTrack(ids[i]);
    if (!t) continue;
    const int y = r.getY() + kHead + (int)i * kRowH;
    if (y + kRowH > r.getBottom()) break;
    const bool rowHover = hover_.row == (int)i;
    g.setColour(rowHover && hover_.col == -1 ? kAccent : kText);
    g.setFont(juce::FontOptions(theme::kFontBody));
    const std::string part = doc::scorePartOf(*t);
    g.drawFittedText(u8(t->name), r.getX() + 10, y, kNameW - 90, kRowH, juce::Justification::centredLeft, 1);
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawFittedText(u8(doc::scorePartName(part)), r.getX() + kNameW - 84, y, 78, kRowH, juce::Justification::centredRight, 1);
    for (size_t k = 0; k < d.sections.size(); ++k) {
      const auto& s = d.sections[k];
      const int a = xAt(s.startTick), b = xAt(s.startTick + s.lengthTicks);
      const bool hot = rowHover && hover_.col == (int)k;
      g.setColour(hot ? kGrid.brighter(0.2f) : kField);
      g.fillRect(juce::Rectangle<int>(a + 1, y + 3, b - a - 2, kRowH - 6));
    }
    g.setColour(kAccent);
    for (const auto& c : t->clips) {
      const int64_t from = std::max<int64_t>(0, c.startTick), to = std::min(total, c.startTick + c.lengthTicks);
      if (to <= from) continue;
      g.fillRect(juce::Rectangle<int>(xAt(from) + 1, y + 6, std::max(2, xAt(to) - xAt(from) - 2), kRowH - 12));
    }
  }
}

void SCorePanel::resized() {
  auto header = getLocalBounds().reduced(kGap, 0).removeFromTop(kHeader).reduced(0, 5);
  close_.setBounds(header.removeFromRight(26));
  header.removeFromRight(12);
  auto place = [&](juce::TextButton& b, theme::InfoDot& dot, int w) {
    dot.setBounds(header.removeFromRight(kDot).withSizeKeepingCentre(kDot, kDot));
    header.removeFromRight(2);
    b.setBounds(header.removeFromRight(w));
    header.removeFromRight(10);
  };
  place(rise_, riseDot_, 140);
  place(arrange_, arrangeDot_, 90);
  place(shape_, shapeDot_, 130);
  const int tw = juce::GlyphArrangement::getStringWidthInt(title_.getFont(), title_.getText()) + 10;
  title_.setBounds(header.removeFromLeft(tw));
  titleDot_.setBounds(header.removeFromLeft(kDot).withSizeKeepingCentre(kDot, kDot));
}

void SCorePanel::mouseMove(const juce::MouseEvent& e) {
  const Cell c = cellAt(e.getPosition());
  if (c.row != hover_.row || c.col != hover_.col) { hover_ = c; repaint(); }
  setMouseCursor(c.row >= 0 && c.col != -2 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void SCorePanel::mouseExit(const juce::MouseEvent&) { hover_ = {}; repaint(); }

void SCorePanel::mouseUp(const juce::MouseEvent& e) {
  const Cell c = cellAt(e.getPosition());
  const auto ids = rows();
  if (c.row < 0 || c.row >= (int)ids.size()) return;
  if (c.col == -1) { if (onOpenTrack) onOpenTrack(ids[(size_t)c.row]); return; }
  const auto& d = host_.currentDocument();
  if (c.col >= 0 && c.col < (int)d.sections.size()) toggle(ids[(size_t)c.row], d.sections[(size_t)c.col].id);
}

void SCorePanel::toggle(const std::string& trackId, const std::string& sectionId) {
  const auto& d = host_.currentDocument();
  const doc::Track* t = d.findTrack(trackId);
  const doc::Section* s = nullptr;
  for (const auto& x : d.sections) if (x.id == sectionId) s = &x;
  if (!t || !s) return;
  const bool on = !doc::scorePlays(*t, *s);
  const juce::String label = u8(t->name) + (on ? u8(" plays in ") : u8(" rests in ")) + u8(s->name);
  host_.applyEdit([&](doc::Document& doc) { doc::scoreSetPlays(doc, trackId, sectionId, on); }, u8("Stage Core: ") + label);
}

void SCorePanel::showShapes() {
  juce::PopupMenu menu;
  int id = 1;
  for (const auto& s : doc::scoreShapes()) menu.addItem(juce::PopupMenu::Item(u8(s.name)).setID(id++));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&shape_), [this](int chosen) {
    if (chosen >= 1 && chosen <= (int)doc::scoreShapes().size()) applyShape(doc::scoreShapes()[(size_t)chosen - 1].id);
  });
}

void SCorePanel::applyShape(const std::string& shapeId) {
  const doc::ScoreShape* s = doc::findScoreShape(shapeId);
  if (!s) return;
  host_.applyEdit([&](doc::Document& d) { doc::setScoreShape(d, shapeId); }, u8("Stage Core: ") + u8(s->name));
  if (onSay) onSay(u8(s->info) + u8(" Now Arrange puts the tracks in."));
}

void SCorePanel::arrange() {
  auto ids = host_.selectedTracks();
  if (ids.size() <= 1) ids = rows();  // one track shown: the whole song
  int written = 0;
  host_.applyEdit([&](doc::Document& d) { written = doc::scoreArrange(d, ids); }, u8("Stage Core: Arrange"));
  if (onSay) onSay(u8("Stage Core: ") + juce::String(written) + u8(" clips placed by the rules. Click a square to change one."));
}

void SCorePanel::rises() {
  int added = 0;
  const auto ids = rows();
  host_.applyEdit([&](doc::Document& d) { added = doc::scoreRises(d, ids); }, u8("Stage Core: Rise on build-ups"));
  if (onSay) onSay(added ? u8("Stage Core: the build-up rise is on ") + juce::String(added) + u8(" build-up(s).")
                         : u8("Stage Core: no track has FX Core in region mode yet (put FX Core · Build-up on a riser first)."));
}

}  // namespace core::app
