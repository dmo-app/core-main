#include "PatternEditor.h"
#include "stutter.hpp"
#include "follow.hpp"
#include "Language.h"
#include "Keymap.h"
#include "BuildupPanel.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "edit.hpp"
#include "groove.hpp"
#include "pattern_tools.hpp"
#include "pcore.hpp"
#include "Plugins.h"
#include "role_tools.hpp"
#include "Theme.h"

namespace core::app {

namespace {
juce::String u8(const char* t) { return juce::String::fromUTF8(t); }
juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }
using theme::kPanel, theme::kText, theme::kMuted;
const juce::Colour kGridDark = theme::kField, kGridLine = theme::kGrid.brighter(0.04f), kBarLine = theme::kOutline.brighter(0.1f),
    kNote = theme::kAccent, kSelected = theme::kAccentBright;
constexpr int kKeys = 30;  // the keyboard at the grid's left edge (not for drums)
constexpr int kHeader = 34, kRuler = 20, kLabels = 70, kVelocity = 56;
// By menu id (1..7); the menu lists them finest first. 1/64 and 1/128 came later (ids 6, 7): saved choices keep their meaning.
const int64_t kSnaps[] = {120, 240, 320, 480, 960, 60, 30};
const char* kSnapNames[] = {"1/32", "1/16", "1/8 trip.", "1/8", "1/4", "1/64", "1/128"};
constexpr int kSnapOrder[] = {7, 6, 1, 2, 3, 4, 5};
// Pattern lengths offered (menu id = index + 1): a beat or two for short loops (a hat figure), then bars.
const int64_t kLengths[] = {doc::kPpq, 2 * doc::kPpq, 4 * doc::kPpq, 8 * doc::kPpq, 16 * doc::kPpq, 32 * doc::kPpq, 64 * doc::kPpq};
constexpr int kLengthCount = 7;
// A length in words: "1 bar", "3 beats", "6 steps" (1/16), never ticks.
juce::String lengthName(int64_t ticks) {
  const auto plural = [](int64_t n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); };
  if (ticks % (4 * doc::kPpq) == 0) return plural(ticks / (4 * doc::kPpq), "bar", "bars");
  if (ticks % doc::kPpq == 0) return plural(ticks / doc::kPpq, "beat", "beats");
  if (ticks % (doc::kPpq / 4) == 0) return plural(ticks / (doc::kPpq / 4), "step", "steps");
  return juce::String(ticks / (double)doc::kPpq, 2) + " beats";
}
// The hat sounds of the built-in hats synth (engine Hats), in the order they are offered.
constexpr int kClosed = 42, kOpen = 46, kPedal = 44, kRide = 51, kShaker = 70;
// Open above closed, as before (other tools take the hats track's last row as its main sound when nothing was added).
const std::vector<int> kHatOrder = {kOpen, kClosed, kPedal, kRide, kShaker};
// The kick track's sounds (engine Kick, 2026-11-08): one per key, each a copy of one of Diogo's kicks; C2 = the knobs.
const std::vector<int> kKickOrder = {41, 40, 39, 38, 37, 36};

constexpr int kAddRow = 24, kDrumVelocity = 40;
constexpr double kPadMin = 22;  // drums: the narrowest step (px) the first view gives; longer patterns page

juce::String pitchName(int p) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return juce::String(names[p % 12]) + juce::String(p / 12 - 1);
}
juce::String drumName(int p) {
  return p == 36 ? juce::String("Kick") : p == kClosed ? u8("Closed hat") : p == kOpen ? u8("Open hat") : p == kPedal ? u8("Pedal hat")
         : p == kRide ? u8("Ride") : p == kShaker ? u8("Shaker") : p == 38 || p == 40 ? u8("Snare") : p == 39 ? u8("Clap") : pitchName(p);
}
juce::String kickName(int p) {
  static const char* names[] = {"Kick", "Kick Long", "Kick Punchy", "Kick Deep", "Kick Tight", "Kick Round"};
  return p >= 36 && p <= 41 ? juce::String(names[p - 36]) : pitchName(p);
}
}  // namespace

PatternEditor::PatternEditor(Host& host) : host_(host) {
  // Which pattern of the track is edited; "New" adds an empty one (place it with a double click on the lane).
  patternBox_.setTooltip(u8("Pattern to edit. Every clip that uses it changes."));
  patternBox_.onChange = [this] {
    const doc::Track* t = currentTrack();
    const int i = patternBox_.getSelectedId() - 1;
    if (t && i >= 0 && i < (int)t->patterns.size()) setPattern(t->patterns[(size_t)i].id);
  };
  newPattern_.setButtonText("New");
  newPattern_.setTooltip(u8("New empty pattern on this track. Double-click the clip lane to place it in the song."));
  newPattern_.onClick = [this] {
    if (!currentTrack()) return;
    const std::string trackId = trackId_;
    const int64_t length = pattern()->lengthTicks;
    std::string id;
    host_.applyEdit([&](doc::Document& d) { id = doc::addPattern(doc::trackById(d, trackId), length); }, u8("New pattern"));
    if (!id.empty()) setPattern(id);
  };
  clearPattern_.setButtonText("Clear");
  clearPattern_.setTooltip(u8("Delete every note of this pattern (its clips stay)"));
  clearPattern_.onClick = [this] {
    if (!currentTrack()) return;
    const std::string trackId = trackId_, patternId = patternId_;
    host_.applyEdit([&](doc::Document& d) { doc::clearPattern(doc::trackById(d, trackId), patternId); }, u8("Clear pattern"));
    selected_.clear();
    repaint();
  };
  removePattern_.setButtonText("Delete");
  removePattern_.setTooltip(u8("Delete this pattern and the clips that play it (a track keeps at least one pattern)"));
  removePattern_.onClick = [this] {
    if (!currentTrack()) return;
    const std::string trackId = trackId_, patternId = patternId_;
    host_.applyEdit([&](doc::Document& d) { doc::removePattern(doc::trackById(d, trackId), patternId); }, u8("Delete pattern"));
  };
  for (int id : kSnapOrder) snap_.addItem(kSnapNames[id - 1], id);
  snap_.setTooltip(u8("Grid (snap) for new notes, moves and Quantize"));
  snap_.setSelectedId(2, juce::dontSendNotification);  // 1/16
  snap_.onChange = [this] { overlay_ = {}; resized(); repaint(); };  // the ghost follows the grid; drums: pads and bars shown too
  for (int i = 0; i < kLengthCount; ++i) length_.addItem(lengthName(kLengths[i]), i + 1);
  length_.onChange = [this] {
    const int id = length_.getSelectedId();
    if (id < 1 || id > kLengthCount || trackId_.empty()) return;
    const int64_t ticks = kLengths[id - 1];
    const std::string trackId = trackId_, patternId = patternId_;
    host_.applyEdit([trackId, patternId, ticks](doc::Document& d) { doc::extendPatternRepeating(doc::patternById(doc::trackById(d, trackId), patternId), ticks); },
                    u8("Pattern length"));
  };
  length_.setTooltip(u8("Pattern length (it repeats along the song). Longer: the notes repeat into the new bars. Shorter: the notes that no longer fit go (Ctrl+Z brings them back)."));
  octaveUp_.setButtonText(u8("Oct +"));
  octaveUp_.setTooltip(u8("One octave up (wheel: one key; Alt+wheel: bigger or smaller keys)"));
  octaveDown_.setButtonText(u8("Oct −"));
  octaveDown_.setTooltip(u8("One octave down (wheel: one key; Alt+wheel: bigger or smaller keys)"));
  octaveUp_.onClick = [this] { lowPitch_ = std::min(128 - keysShown(), lowPitch_ + 12); repaint(); };
  octaveDown_.onClick = [this] { lowPitch_ = std::max(0, lowPitch_ - 12); repaint(); };
  for (auto [b, icon, tip, t] : {std::tuple<juce::TextButton*, const char*, const char*, Tool>{&drawTool_, "cursor", "Draw: a click adds a note, a drag moves it; right-click deletes", Tool::Draw},
                                 {&selectTool_, "select", "Select: drag a box around notes (Ctrl adds to the selection)", Tool::Select},
                                 {&eraseTool_, "erase", "Erase: click or drag over notes (with any tool: right-click)", Tool::Erase}}) {
    b->getProperties().set(theme::kIconProperty, icon);
    b->setTooltip(tip);
    b->setClickingTogglesState(false);
    b->onClick = [this, t] { setTool(t); };
  }
  // One Edit menu for every note tool (Diogo, 2026-11-05): Timing, Notes, Variation, Randomize, More/Fewer notes,
  // Build-up, each a submenu; the windows (P-Core, Build-up) and Save at the end.
  edit_.setButtonText(u8("Edit ▾"));
  edit_.onClick = [this] { showTools(); };
  editInfo_.setTooltip(u8("Every note tool in one menu. It works on the bars shown, or only on the selected notes when some "
                          "are selected. Each choice is one Ctrl+Z."));
  toast_.setColour(juce::Label::backgroundColourId, theme::kField);
  toast_.setColour(juce::Label::outlineColourId, theme::kAccent);
  toast_.setColour(juce::Label::textColourId, kText);
  toast_.setJustificationType(juce::Justification::centredLeft);
  toast_.setBorderSize(juce::BorderSize<int>(4, 10, 4, 10));
  toast_.setMinimumHorizontalScale(1.0f);
  toast_.setInterceptsMouseClicks(false, false);
  addChildComponent(toast_);
  title_.setColour(juce::Label::textColourId, kText);
  title_.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
  hint_.setColour(juce::Label::textColourId, kMuted);
  hint_.setFont(juce::FontOptions(theme::kFontBody));
  hint_.setJustificationType(juce::Justification::centredRight);
  pagePrev_.getProperties().set(theme::kIconProperty, "left");  // clean chevrons, not the ‹ › characters
  pageNext_.getProperties().set(theme::kIconProperty, "right");
  pagePrev_.setTooltip(u8("Previous bars (Shift+wheel scrolls; Ctrl+wheel or − shows more)"));
  pageNext_.setTooltip(u8("Next bars (Shift+wheel scrolls; Ctrl+wheel or − shows more)"));
  pagePrev_.onClick = [this] { viewStart_ = shownStart() - shownLength(); placePaging(); repaint(); };
  pageNext_.onClick = [this] { viewStart_ = shownStart() + shownLength(); placePaging(); repaint(); };
  addSound_.setButtonText(u8("+ Sound"));
  addSound_.setTooltip(u8("Add a row for another sound: pedal hat, ride or shaker; on a Kick, the other kicks (each a copy of a library kick)"));
  addSound_.onClick = [this] {
    juce::PopupMenu m;
    for (int p : hiddenSounds()) m.addItem(p, soundName(p));
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addSound_), [safe = juce::Component::SafePointer<PatternEditor>(this)](int r) {
      if (!safe || r <= 0) return;
      safe->addedRows_[safe->trackId_].insert(r);
      safe->placeAddSound();
      safe->repaint();
    });
  };
  zoomIn_.setButtonText("+");
  zoomOut_.setButtonText(u8("−"));
  zoomIn_.setTooltip(u8("Zoom in (Ctrl+wheel)"));
  zoomOut_.setTooltip(u8("Zoom out (Ctrl+wheel)"));
  zoomIn_.onClick = [this] { zoom(0.5, shownStart() + shownLength() / 2); };
  zoomOut_.onClick = [this] { zoom(2.0, shownStart() + shownLength() / 2); };
  for (auto* c : std::initializer_list<juce::Component*>{&zoomIn_, &zoomOut_, &patternBox_, &newPattern_, &clearPattern_, &removePattern_, &snap_, &length_, &octaveUp_, &octaveDown_, &title_, &hint_, &addSound_, &pagePrev_, &pageNext_,
                                                                &drawTool_, &selectTool_, &eraseTool_, &edit_, &editInfo_}) {
    c->setWantsKeyboardFocus(false);
    addAndMakeVisible(c);
  }
  setWantsKeyboardFocus(true);
  addChildComponent(dock_);
  dock_.onLayoutChanged = [this] { resized(); repaint(); };
  setTool(Tool::Draw);
  grooveSeed_ = (uint64_t)juce::Random::getSystemRandom().nextInt64();
  documentChanged();
}

PatternEditor::~PatternEditor() = default;

void PatternEditor::setTool(Tool tool) {
  tool_ = tool;
  if (!overlay_.area().isEmpty()) repaint(overlay_.area().expanded(2));
  overlay_ = {};  // the ghost is the draw tool's
  drawTool_.setToggleState(tool == Tool::Draw, juce::dontSendNotification);
  selectTool_.setToggleState(tool == Tool::Select, juce::dontSendNotification);
  eraseTool_.setToggleState(tool == Tool::Erase, juce::dontSendNotification);
  setMouseCursor(tool == Tool::Draw ? juce::MouseCursor::NormalCursor : tool == Tool::Select ? juce::MouseCursor::CrosshairCursor
                                                                                            : juce::MouseCursor::PointingHandCursor);
}

void PatternEditor::setDrawingHelp(bool ghost, bool guides) {
  if (ghost == showGhost_ && guides == showGuides_) return;
  showGhost_ = ghost;
  showGuides_ = guides;
  if (!overlay_.area().isEmpty()) repaint(overlay_.area().expanded(2));
  overlay_ = {};
}

// The guides for a note drawn at `note` (tick..end, pitch): across, from the nearest other note on the same key
// to this one; down, from the nearest other note that starts where this one starts (or ends where it starts) to
// this one. Notes in `skip` (the ones being moved) don't count. One pass over the notes, nothing allocated.
PatternEditor::Overlay PatternEditor::guidesFor(juce::Rectangle<int> note, int64_t tick, int64_t end, int pitch,
                                                const std::set<std::string>& skip) const {
  Overlay o;
  if (!showGuides_ || note.isEmpty() || !currentTrack()) return o;
  juce::Rectangle<int> sameKey, sameTime;
  int sameKeyGap = std::numeric_limits<int>::max(), sameTimeGap = sameKeyGap, downX = 0;
  for (const auto& n : pattern()->notes) {
    if (skip.count(n.id) || (n.tick == tick && n.pitch == pitch)) continue;
    const bool key = n.pitch == pitch, start = n.tick == tick, touch = n.tick + n.length == tick, after = n.tick == end;
    if (!key && !start && !touch && !after) continue;
    const auto b = noteBounds(n);
    if (b.isEmpty()) continue;
    if (key) {
      const int gap = std::abs(b.getCentreX() - note.getCentreX());
      if (gap < sameKeyGap) { sameKeyGap = gap; sameKey = b; }
    } else {
      const int gap = std::abs(b.getCentreY() - note.getCentreY());
      if (gap < sameTimeGap) { sameTimeGap = gap; sameTime = b; downX = start || touch ? note.getX() : note.getRight(); }
    }
  }
  const auto area = grid();
  if (!sameKey.isEmpty()) {
    const int x0 = std::min(sameKey.getX(), note.getX()), x1 = std::max(sameKey.getRight(), note.getRight());
    o.across = juce::Rectangle<int>(x0, note.getCentreY(), x1 - x0, 1).getIntersection(area);
  }
  if (!sameTime.isEmpty()) {
    const int y0 = std::min(sameTime.getY(), note.getY()), y1 = std::max(sameTime.getBottom(), note.getBottom());
    o.down = juce::Rectangle<int>(downX, y0, 1, y1 - y0).getIntersection(area.expanded(1, 0));
  }
  return o;
}

// Under the mouse with the draw tool, on an empty spot of the grid: where a click puts the note, and its guides.
PatternEditor::Overlay PatternEditor::hoverOverlay(juce::Point<int> p) const {
  if ((!showGhost_ && !showGuides_) || tool_ != Tool::Draw || !currentTrack() || !grid().contains(p) || noteAt(p, nullptr)) return {};
  doc::Note n;
  n.tick = tickAtX(p.x, true);
  n.pitch = pitchAtY(p.y);
  n.length = std::min<int64_t>(pattern()->lengthTicks, drums() ? std::min<int64_t>(snapTicks(), 240) : snapTicks());  // as clickCell
  if (n.pitch < 0) return {};
  const auto b = noteBounds(n);
  Overlay o = guidesFor(b, n.tick, n.tick + n.length, n.pitch, {});
  if (showGhost_) o.ghost = b;
  return o;
}

PatternEditor::Overlay PatternEditor::dragOverlay() const {
  if (drag_ != Drag::Move || !currentTrack()) return {};
  for (const auto& n : pattern()->notes)
    if (n.id == dragNote_) return guidesFor(noteBounds(n), n.tick, n.tick + n.length, n.pitch, selected_);
  return {};
}

juce::Rectangle<int> PatternEditor::rowStrip(int pitch) const {
  if (pitch < 0) return {};
  const auto r = rows();
  const auto it = std::find(r.begin(), r.end(), pitch);
  if (it == r.end()) return {};
  const auto area = grid();
  const int y = area.getY() + (int)std::round((it - r.begin()) * rowHeight());
  return {area.getX() - kLabels, y - 1, area.getWidth() + kLabels + 1, (int)std::ceil(rowHeight()) + 2};
}

// The mouse moved: only the rows whose highlight changed and the old and new overlay are redrawn.
void PatternEditor::updateHover(juce::Point<int> p) {
  const int pitch = currentTrack() && grid().withTrimmedLeft(-kLabels).contains(p) ? pitchAtY(p.y) : -1;
  if (pitch != hoverPitch_) {
    repaint(rowStrip(hoverPitch_));
    repaint(rowStrip(pitch));
    hoverPitch_ = pitch;
  }
  const Overlay o = drag_ == Drag::None ? hoverOverlay(p) : Overlay{};
  if (o == overlay_) return;
  if (!overlay_.area().isEmpty()) repaint(overlay_.area().expanded(2));
  if (!o.area().isEmpty()) repaint(o.area().expanded(2));
  overlay_ = o;
}

// One accent colour, kept faint: the ghost a dashed outline over a light fill, the guides thin dashes.
void PatternEditor::paintOverlay(juce::Graphics& g, const Overlay& o) const {
  const float dashes[] = {3.0f, 3.0f};
  g.setColour(theme::kAccentBright.withAlpha(0.55f));
  if (!o.across.isEmpty())
    g.drawDashedLine({(float)o.across.getX(), o.across.getY() + 0.5f, (float)o.across.getRight(), o.across.getY() + 0.5f}, dashes, 2, 1.0f);
  if (!o.down.isEmpty())
    g.drawDashedLine({o.down.getX() + 0.5f, (float)o.down.getY(), o.down.getX() + 0.5f, (float)o.down.getBottom()}, dashes, 2, 1.0f);
  if (!o.ghost.isEmpty()) {
    const auto b = o.ghost.toFloat();
    g.setColour(kNote.withAlpha(0.18f));
    g.fillRoundedRectangle(b, 2);
    g.setColour(theme::kAccentBright.withAlpha(0.6f));
    for (auto line : {juce::Line<float>(b.getTopLeft(), b.getTopRight()), juce::Line<float>(b.getTopRight(), b.getBottomRight()),
                      juce::Line<float>(b.getBottomRight(), b.getBottomLeft()), juce::Line<float>(b.getBottomLeft(), b.getTopLeft())})
      g.drawDashedLine(line.withShortenedStart(0.5f).withShortenedEnd(0.5f), dashes, 2, 1.0f);
  }
}

const doc::Track* PatternEditor::currentTrack() const {
  const doc::Track* t = trackId_.empty() ? nullptr : host_.currentDocument().findTrack(trackId_);
  return t && !t->patterns.empty() ? t : nullptr;  // an audio track: no notes to edit
}

const doc::Pattern* PatternEditor::pattern() const {
  const doc::Track* t = currentTrack();
  if (!t) return nullptr;
  const doc::Pattern* p = t->findPattern(patternId_);
  return p ? p : &t->patterns.front();
}

void PatternEditor::setPattern(const std::string& patternId) {
  if (patternId == patternId_) return;
  patternId_ = patternId;
  selected_.clear();
  documentChanged();
}

namespace {
// What kind of drum track it is: "kick", "hats", "snare", "drums", or "" (not a drum track). The old Core instrument's
// name when the song has one; else the track's id or name (Core's sound comes from plugins since 2026-11-16, so a
// plugin kick is a kick by its name: "kick", "kick-2", "Kick layer").
std::string drumRole(const doc::Track& t) {
  for (const char* r : {"kick", "hats", "snare", "drums"})
    if (t.instrument.name == r) return r;
  std::string words = t.id + " " + t.name;
  for (auto& c : words) c = (char)std::tolower((unsigned char)c);
  if (words.find("kick") != std::string::npos) return "kick";
  if (words.find("hat") != std::string::npos) return "hats";
  if (words.find("snare") != std::string::npos || words.find("clap") != std::string::npos) return "snare";
  if (words.find("drum") != std::string::npos) return "drums";
  return {};
}
}  // namespace

bool PatternEditor::drums() const {
  const doc::Track* t = currentTrack();
  return t && !drumRole(*t).empty();
}

bool PatternEditor::hatSounds() const {
  const doc::Track* t = currentTrack();
  const std::string r = t ? drumRole(*t) : std::string();
  return r == "hats" || r == "drums" || r == "kick";
}

// The sounds a drum track offers as rows, top to bottom: the hats' or the kick's.
const std::vector<int>& PatternEditor::soundOrder() const {
  static const std::vector<int> none;
  const doc::Track* t = currentTrack();
  if (!t) return none;
  const std::string r = drumRole(*t);
  return r == "kick" ? kKickOrder : r == "hats" || r == "drums" ? kHatOrder : none;
}

juce::String PatternEditor::soundName(int pitch) const {
  const doc::Track* t = currentTrack();
  return t && t->instrument.name == "kick" ? kickName(pitch) : drumName(pitch);
}

std::vector<int> PatternEditor::rows() const {
  const doc::Track* t = currentTrack();
  if (!t) return {};
  if (t->instrument.name == "snare") return {38, 39};  // Core's own snare and clap
  if (hatSounds()) {
    std::set<int> used;
    for (const auto& n : pattern()->notes) used.insert(n.pitch);
    const auto added = addedRows_.find(trackId_);
    std::vector<int> out;
    const bool kick = t->instrument.name == "kick";
    for (int p : soundOrder())
      if ((kick ? p == 36 : p == kClosed || p == kOpen) || used.count(p) || (added != addedRows_.end() && added->second.count(p))) out.push_back(p);
    if (t->instrument.name == "drums") out.push_back(36);  // the kick at the bottom, as before
    return out;
  }
  std::vector<int> out;
  const int top = std::min(127, lowPitch_ + keysShown() - 1);
  for (int p = top; p >= std::max(0, top - keysShown() + 1); --p) out.push_back(p);
  return out;
}

// Keys shown: what Alt+wheel asked for, or as many as fit at 14 px each (at least an octave, at most five).
int PatternEditor::keysShown() const {
  if (rowsWanted_ > 0) return rowsWanted_;
  const int h = getHeight() - 16 - 34 - 20 - 56 - 6;  // the grid's height (see grid())
  return std::clamp(h / 14, 13, 61);
}

std::vector<int> PatternEditor::hiddenSounds() const {
  std::vector<int> out;
  if (!hatSounds()) return out;
  const auto shown = rows();
  for (int p : soundOrder()) if (std::find(shown.begin(), shown.end(), p) == shown.end()) out.push_back(p);
  return out;
}

void PatternEditor::removeAddedRow(int pitch) {
  auto it = addedRows_.find(trackId_);
  if (it != addedRows_.end()) it->second.erase(pitch);
  if (hoverPitch_ == pitch) hoverPitch_ = -1;
  placeAddSound();
  repaint();
}

juce::Rectangle<int> PatternEditor::rowLabel(int pitch) const {
  const auto r = rows();
  const auto it = std::find(r.begin(), r.end(), pitch);
  if (it == r.end()) return {};
  const auto g = grid();
  const float rowH = rowHeight();
  return {g.getX() - kLabels, g.getY() + (int)std::round((it - r.begin()) * rowH), kLabels - 6, (int)rowH};
}

// An added sound with no steps in this pattern (a sound with steps stays: delete them first).
bool PatternEditor::removableRow(int pitch) const {
  const auto added = addedRows_.find(trackId_);
  if (added == addedRows_.end() || !added->second.count(pitch) || pitch == kClosed || pitch == kOpen) return false;
  for (const auto& n : pattern()->notes) if (n.pitch == pitch) return false;
  return true;
}

juce::Rectangle<int> PatternEditor::removeMark(int pitch) const {
  const auto l = rowLabel(pitch);
  return l.withWidth(14).withSizeKeepingCentre(14, 14);
}

// "+ Sound" sits under the last row, in the names' column (only while a sound is left to add).
void PatternEditor::placeAddSound() {
  const bool show = hatSounds() && !hiddenSounds().empty();
  addSound_.setVisible(show);
  if (!show) return;
  const auto g = grid();
  addSound_.setBounds(g.getX() - kLabels, g.getBottom() + 3, kLabels - 6, kAddRow - 4);
}

int64_t PatternEditor::snapTicks() const { return kSnaps[std::clamp(snap_.getSelectedId(), 1, 7) - 1]; }

void PatternEditor::setTrack(const std::string& trackId, const std::string& patternId) {
  if (trackId == trackId_ && (patternId.empty() || patternId == patternId_)) return;
  const bool otherTrack = trackId != trackId_;
  trackId_ = trackId;
  viewStart_ = viewLength_ = 0;  // another pattern: all of it (drums: the first bars that fit as pads)
  const doc::Track* track = currentTrack();
  patternId_ = !patternId.empty() ? patternId : track ? track->patterns.front().id : std::string();
  selected_.clear();
  if (const doc::Track* t = currentTrack(); t && !drums() && !pattern()->notes.empty()) {
    int low = 127;
    for (const auto& n : pattern()->notes) low = std::min(low, n.pitch);
    lowPitch_ = std::clamp(low - 2, 0, 128 - keysShown());
  }
  documentChanged();
  // A docked tool follows the track shown: built again for it, or closed (no track, or Attachments' notes).
  if (dock_.hasTool() && (otherTrack || !reopenDock_)) {
    if (currentTrack() && reopenDock_ && doc::playsPatterns(*currentTrack())) {
      const bool folded = dock_.folded();
      auto again = reopenDock_;
      dock_.close();  // when it cannot open for this track (it says why), nothing stale stays
      again();
      if (folded) dock_.setFolded(true);
    } else {
      reopenDock_ = nullptr;
      dock_.close();
    }
  }
}

void PatternEditor::documentChanged() {
  overlay_ = {};  // stale after an edit, undo or another track (the whole editor is redrawn below)
  const doc::Track* t = currentTrack();
  if (!t) { trackId_.clear(); patternId_.clear(); selected_.clear(); }
  else {
    if (!t->findPattern(patternId_)) { patternId_ = t->patterns.front().id; selected_.clear(); }
    std::set<std::string> still;
    for (const auto& n : pattern()->notes) if (selected_.count(n.id)) still.insert(n.id);
    selected_.swap(still);
  }
  // The title holds only the track's name; how the pattern is used goes in the tooltip, so it is never cut.
  title_.setText(currentTrack() ? editorName() + u8(" — ") + u8(currentTrack()->name) : trackLabel(), juce::dontSendNotification);
  resized();  // the title's text depends on the room it has
  title_.setTooltip(currentTrack() ? trackLabel() : juce::String());
  patternBox_.clear(juce::dontSendNotification);
  if (t)
    for (size_t i = 0; i < t->patterns.size(); ++i) {
      patternBox_.addItem(u8("Pattern ") + u8(t->patterns[i].name), (int)i + 1);
      if (t->patterns[i].id == patternId_) patternBox_.setSelectedId((int)i + 1, juce::dontSendNotification);
    }
  patternBox_.setEnabled(t != nullptr);
  newPattern_.setEnabled(t != nullptr);
  clearPattern_.setEnabled(t != nullptr && !pattern()->notes.empty());
  removePattern_.setEnabled(t != nullptr && t->patterns.size() > 1);
  int lengthId = 0;
  if (t) for (int i = 0; i < kLengthCount; ++i) if (pattern()->lengthTicks == kLengths[i]) lengthId = i + 1;
  length_.setSelectedId(lengthId, juce::dontSendNotification);
  if (t && lengthId == 0) length_.setText(lengthName(pattern()->lengthTicks), juce::dontSendNotification);
  const bool editable = t != nullptr;
  snap_.setEnabled(editable);
  length_.setEnabled(editable);
  octaveUp_.setVisible(editable && !drums());
  octaveDown_.setVisible(editable && !drums());
  setTooltip(!editable ? juce::String()
             : drums() ? u8("Click: step on/off · Drag: move, also to another sound (Shift: off the grid) · Wheel over a step or the velocity lane: how loud")
                       : u8("Click: note · Drag: move (Shift: off the grid) · Right edge: length · Wheel or velocity lane: velocity · Del · Ctrl+D duplicate"));
  for (auto* b : {&drawTool_, &selectTool_, &eraseTool_, &edit_}) b->setEnabled(editable);
  placeAddSound();
  hint_.setText(!editable ? juce::String()
                : drums() ? u8("Click: step on/off · Drag: move (Shift: off the grid) · Wheel over a note: velocity")
                          : u8("Click: note · Drag: move (Shift: off the grid) · Right edge: length · Wheel: velocity · Del · Ctrl+D duplicate"),
                juce::dontSendNotification);
  resized();  // the pattern menu fits its (new) name
  repaint();
}

juce::String PatternEditor::trackLabel() const {
  const doc::Track* t = currentTrack();
  if (!t) return u8("Choose a note track to edit its pattern. Audio clips: drag moves, the edges trim, right-click splits.");
  const doc::Pattern* p = pattern();
  const int uses = t->clipsUsing(p->id);
  return u8(t->name) + (uses > 1 ? u8(" · in ") + juce::String(uses) + " clips" : uses == 0 ? u8(" · not in the song") : juce::String());
}

void PatternEditor::resized() {
  dock_.setBounds(dock_.hasTool() ? getLocalBounds().reduced(8).removeFromRight(dockWidth() - 8) : juce::Rectangle<int>());
  auto header = area().reduced(8, 4).removeFromTop(kHeader - 4);
  // Right side first (grid, length, octave), then the left from the start; the title takes what is left.
  zoomIn_.setBounds(header.removeFromRight(30).reduced(2));
  zoomOut_.setBounds(header.removeFromRight(30).reduced(2));
  header.removeFromRight(6);
  octaveUp_.setBounds(header.removeFromRight(50).reduced(2));
  octaveDown_.setBounds(header.removeFromRight(50).reduced(2));
  length_.setBounds(header.removeFromRight(84).reduced(2));
  snap_.setBounds(header.removeFromRight(88).reduced(2));
  header.removeFromRight(8);
  editInfo_.setBounds(header.removeFromRight(theme::InfoDot::kSlot).withSizeKeepingCentre(theme::InfoDot::kSlot, theme::InfoDot::kSlot));
  edit_.setBounds(header.removeFromRight(66).reduced(2));
  header.removeFromRight(8);
  for (auto* b : {&eraseTool_, &selectTool_, &drawTool_}) b->setBounds(header.removeFromRight(30).reduced(2));
  header.removeFromRight(8);
  removePattern_.setBounds(header.removeFromRight(56).reduced(2));
  clearPattern_.setBounds(header.removeFromRight(52).reduced(2));
  newPattern_.setBounds(header.removeFromRight(48).reduced(2));
  // The pattern's whole name when there is room (up to 260 px), never under 112; the title keeps at least 60.
  const int nameW = juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(theme::kFontStrong)), patternBox_.getText()) + 44;
  patternBox_.setBounds(header.removeFromRight(std::min(std::clamp(nameW, 112, 260), std::max(0, header.getWidth() - 60))).reduced(2));
  title_.setBounds(header.withSizeKeepingCentre(header.getWidth(), 18));  // one line: a long name ends in "..."
  // A narrow header drops the "PIANO ROLL" / "STEPS" prefix so the track's name is what stays readable.
  if (const doc::Track* t = currentTrack()) {
    const juce::String full = editorName() + u8(" — ") + u8(t->name);
    const bool fits = juce::GlyphArrangement::getStringWidthInt(title_.getFont(), full) + 8 <= header.getWidth();
    title_.setText(fits ? full : u8(t->name), juce::dontSendNotification);
  }
  hint_.setBounds({});
  hint_.setVisible(false);  // the hint is the editor's tooltip
  placeAddSound();
  placePaging();
}

juce::String PatternEditor::editorName() const { return drums() ? u8("STEPS") : u8("PIANO ROLL"); }

int PatternEditor::dockWidth() const {
  const int w = dock_.wantedWidth(getWidth() - 16 - kLabels);
  return w > 0 ? w + 8 : 0;
}

juce::Rectangle<int> PatternEditor::area() const { return getLocalBounds().withTrimmedRight(dockWidth()); }

void PatternEditor::dockTool(std::unique_ptr<juce::Component> tool, const juce::String& title, std::function<void()> reopen) {
  reopenDock_ = std::move(reopen);
  dock_.show(std::move(tool), title);
}


// ‹ › in the corner over the names, while the bars shown are not the whole pattern.
void PatternEditor::placePaging() {
  const double len = currentTrack() ? (double)pattern()->lengthTicks : 0.0;
  const bool paging = currentTrack() && shownLength() < len - 1;
  pagePrev_.setVisible(paging);
  pageNext_.setVisible(paging);
  if (!paging) return;
  const auto g = grid();
  const juce::Rectangle<int> corner(g.getX() - kLabels, g.getY() - kRuler, kLabels - 8, kRuler - 2);
  pagePrev_.setBounds(corner.withWidth(corner.getWidth() / 2).reduced(1, 0));
  pageNext_.setBounds(corner.withTrimmedLeft(corner.getWidth() / 2).reduced(1, 0));
  pagePrev_.setEnabled(shownStart() > 0.5);
  pageNext_.setEnabled(shownStart() + shownLength() < len - 0.5);
}

// Drums, before any zoom: whole bars (1, 2, 4...) that fit with each grid step at least kPadMin wide, so the
// steps are pads you can click and not hairlines; a longer pattern is paged with ‹ ›.
double PatternEditor::drumFitLength() const {
  const double width = std::max(1, area().getWidth() - 16 - kLabels);
  const double bar = 4.0 * doc::kPpq, stepsPerBar = bar / (double)std::max<int64_t>(snapTicks(), doc::kPpq / 4);
  double bars = 1;
  while (bars * 2 * stepsPerBar * kPadMin <= width) bars *= 2;
  return bars * bar;
}

// Drum tracks: the rows take what they need (up to 44 px each) from the top, "+ Sound" and the velocity lane
// right under them, so the lanes are not lost in empty space. Pitched tracks: the grid fills, velocity at the bottom.
juce::Rectangle<int> PatternEditor::grid() const {
  const auto all = area().reduced(8).withTrimmedTop(kHeader + kRuler).withTrimmedLeft(kLabels);
  if (!drums()) return all.withTrimmedBottom(kVelocity + 6);
  // Pads about as tall as they are wide (28 to 64 px), as many rows as fit.
  const int room = all.getHeight() - kAddRow - kDrumVelocity - 6;
  const int n = std::max(1, (int)rows().size());
  const double step = all.getWidth() * (double)std::max<int64_t>(snapTicks(), doc::kPpq / 4) / std::max(1.0, shownLength());
  const int padMax = std::clamp((int)std::round(step) + 4, 28, 64);
  const int rowH = std::clamp(room / n, 12, padMax);
  return all.withHeight(std::max(0, std::min(room, rowH * n)));
}

juce::Rectangle<int> PatternEditor::velocityLane() const {
  if (drums()) {
    const auto g = grid();
    return {g.getX(), g.getBottom() + kAddRow + 6, g.getWidth(), kDrumVelocity};
  }
  return area().reduced(8).removeFromBottom(kVelocity).withTrimmedLeft(kLabels);
}

// The part of the pattern shown (zoom): all of it, or viewLength_ ticks from viewStart_ (Ctrl+wheel).
double PatternEditor::shownLength() const {
  const double len = currentTrack() ? (double)pattern()->lengthTicks : 1.0;
  if (viewLength_ > 0) return std::min(viewLength_, len);
  return drums() ? std::min(len, drumFitLength()) : len;
}

double PatternEditor::shownStart() const {
  const double len = currentTrack() ? (double)pattern()->lengthTicks : 1.0;
  return std::clamp(viewStart_, 0.0, std::max(0.0, len - shownLength()));
}

int PatternEditor::xOfTick(int64_t tick) const {
  const auto g = grid();
  return g.getX() + (int)std::round(g.getWidth() * ((double)tick - shownStart()) / shownLength());
}

// Where the playhead line is drawn (the song's position inside the pattern); -1: not in view.
int PatternEditor::playheadX() const {
  if (!currentTrack() || pattern()->lengthTicks <= 0) return -1;
  const double at = std::fmod(std::max(0.0, host_.playheadTick()), (double)pattern()->lengthTicks);
  const auto area = grid();
  const int x = xOfTick((int64_t)at);
  return x >= area.getX() && x <= area.getRight() ? x : -1;
}

// The start line to the ruler's tick under x (to the grid), in the pass of the pattern the song is in now.
void PatternEditor::movePlayheadTo(int x) {
  if (!currentTrack() || pattern()->lengthTicks <= 0) return;
  host_.movePlayhead(doc::songTickInPattern((int64_t)host_.playheadTick(), pattern()->lengthTicks, tickAtX(x, true)));
}

// Called every frame: the strips under the old and the new line are redrawn, not the whole piano roll.
void PatternEditor::playheadMoved() {
  const int x = playheadX();
  if (x == playheadX_) return;
  const auto area = grid();
  for (const int at : {playheadX_, x})
    if (at >= 0) repaint(at - 5, area.getY() - kRuler, 12, area.getHeight() + kRuler);
  playheadX_ = x;
}

int64_t PatternEditor::tickAtX(int x, bool snap) const {
  const doc::Track* t = currentTrack();
  if (!t) return 0;
  const auto g = grid();
  const double f = std::clamp((x - g.getX()) / (double)std::max(1, g.getWidth()), 0.0, 1.0);
  const int64_t tick = std::min<int64_t>(pattern()->lengthTicks, (int64_t)(shownStart() + f * shownLength()));
  if (!snap) return tick;
  const int64_t s = snapTicks();
  return std::max<int64_t>(0, std::min(pattern()->lengthTicks - s, tick / s * s));  // the step that contains the click (never before 0, also when the grid is longer than a short pattern)
}

juce::Point<int> PatternEditor::cellGrabPoint(int64_t tick, int pitch) const {
  doc::Note n;
  n.tick = tick;
  n.pitch = pitch;
  n.length = std::min<int64_t>(snapTicks(), 240);
  const auto b = noteBounds(n);
  return {b.getX() + 2, b.getCentreY()};
}

// A row's height: the grid shared by the rows (drums: grid() is already sized to whole rows).
float PatternEditor::rowHeight() const {
  const size_t n = std::max<size_t>(1, rows().size());
  return (float)grid().getHeight() / (float)n;
}

int PatternEditor::pitchAtY(int y) const {
  const auto r = rows();
  if (r.empty()) return -1;
  const auto g = grid();
  const int i = std::clamp((int)((y - g.getY()) / std::max(1.0f, rowHeight())), 0, (int)r.size() - 1);
  return r[(size_t)i];
}

juce::Rectangle<int> PatternEditor::noteBounds(const doc::Note& n) const {
  const auto r = rows();
  const auto it = std::find(r.begin(), r.end(), n.pitch);
  if (it == r.end()) return {};
  const auto g = grid();
  const float rowH = rowHeight();
  const int y = g.getY() + (int)std::round((it - r.begin()) * rowH);
  if (drums()) {  // a step: its cell (the step's length, 1/16 or the finer grid), a gap around it
    const int x0 = xOfTick(n.tick), x1 = std::max(x0 + 4, xOfTick(n.tick + n.length));
    const int gap = x1 - x0 >= 12 ? 2 : 1;
    return {x0 + gap, y + 2, std::max(2, x1 - x0 - 2 * gap + 1), std::max(2, (int)rowH - 4)};
  }
  const int x0 = xOfTick(n.tick), x1 = std::max(x0 + 3, xOfTick(n.tick + n.length));
  return {x0, y + 1, x1 - x0, std::max(2, (int)rowH - 2)};
}

const doc::Note* PatternEditor::noteAt(juce::Point<int> p, bool* onRightEdge) const {
  const doc::Track* t = currentTrack();
  if (!t) return nullptr;
  for (auto it = pattern()->notes.rbegin(); it != pattern()->notes.rend(); ++it) {
    const auto b = noteBounds(*it);
    if (b.contains(p)) {
      if (onRightEdge) *onRightEdge = !drums() && p.x >= b.getRight() - 6 && b.getWidth() > 10;
      return &*it;
    }
  }
  return nullptr;
}

void PatternEditor::paint(juce::Graphics& g) {
  g.setColour(kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
  const doc::Track* t = currentTrack();
  if (!t) return;
  const auto area = grid();
  const auto r = rows();
  const float rowH = rowHeight();
  // The ruler: bars and beats of the pattern (bar.beat), the playhead's mark.
  {
    const auto ruler = area.withY(area.getY() - kRuler).withHeight(kRuler);
    g.saveState();
    g.reduceClipRegion(ruler.expanded(1, 0));
    const bool beats = xOfTick(doc::kPpq) - xOfTick(0) >= 30;
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (int64_t tick = 0; tick < pattern()->lengthTicks; tick += beats ? doc::kPpq : 4 * doc::kPpq) {
      const bool bar = tick % (4 * doc::kPpq) == 0;
      const int x = xOfTick(tick);
      g.setColour(bar ? kBarLine : kGridLine.brighter(0.2f));
      g.fillRect(x, ruler.getBottom() - (bar ? 8 : 5), 1, bar ? 8 : 5);
      g.setColour(bar ? kText.withAlpha(0.85f) : kMuted);
      const juce::String label = juce::String(tick / (4 * doc::kPpq) + 1) + (beats ? "." + juce::String(tick / doc::kPpq % 4 + 1) : juce::String());
      g.drawText(label, x + 3, ruler.getY(), 40, ruler.getHeight() - 6, juce::Justification::centredLeft, false);
    }
    g.restoreState();
  }
  // Rows and their names; a keyboard for pitched tracks (black keys shorter, over the white ones).
  g.setFont(juce::FontOptions(theme::kFontSmall));
  for (size_t i = 0; i < r.size(); ++i) {
    const int y = area.getY() + (int)std::round(i * rowH);
    const bool black = !drums() && juce::String("0101001010100").substring(r[i] % 12, r[i] % 12 + 1) == "1";
    g.setColour(black ? kGridDark : kGridDark.brighter(0.05f));
    g.fillRect(area.getX(), y, area.getWidth(), (int)std::ceil(rowH));
    int labelRight = area.getX() - 6;
    if (!drums()) {
      const float kx = (float)(area.getX() - kKeys - 1), h = std::max(1.0f, rowH);
      g.setColour(juce::Colour(0xffd5dae2));
      g.fillRect(kx, (float)y, (float)kKeys, h);
      if (black) {
        g.setColour(juce::Colour(0xff1c2027));
        g.fillRoundedRectangle(kx, (float)y + 0.5f, kKeys * 0.62f, h - 1.0f, 1.5f);
      }
      // A line between white keys where no black key sits (B|C and E|F), and under every C.
      const int pc = r[i] % 12;
      if (pc == 0 || pc == 5) {
        g.setColour(juce::Colour(0xff9aa1ac));
        g.fillRect(kx, (float)y + h - 1, (float)kKeys, 1.0f);
      }
      labelRight -= kKeys + 1;
    }
    // Names: every row when there is room, every white key from 9 px, otherwise C, E and G (never two
    // names touching); the row under the mouse always, in the highlight colour, so you see where you are.
    const bool hovered = r[i] == hoverPitch_;
    if (hovered) {
      g.setColour(kNote.withAlpha(0.10f));
      g.fillRect(area.getX(), y, area.getWidth(), (int)std::ceil(rowH));
    }
    const int pc = r[i] % 12;
    if (drums() || hovered || pc == 0 || rowH >= 11 || (rowH >= 9 && !black) || (rowH >= 5.5f && (pc == 4 || pc == 7))) {
      g.setColour(hovered ? kNote : pc == 0 && !drums() ? kText.withAlpha(0.85f) : kMuted);
      g.setFont(juce::FontOptions(juce::jlimit(9.0f, theme::kFontSmall, rowH + 1.5f)));
      g.drawText(drums() ? soundName(r[i]) : pitchName(r[i]), area.getX() - kLabels, y, labelRight - (area.getX() - kLabels), (int)rowH,
                 juce::Justification::centredRight);
    }
    // A sound added with "+ Sound" and still empty: a small ✕ at the left of its name hides the row again.
    if (hovered && removableRow(r[i])) {
      g.setColour(kMuted);
      g.drawText(u8("✕"), removeMark(r[i]), juce::Justification::centred);
    }
  }
  // Grid lines and notes stay inside the grid (zoomed in, the rest of the pattern is off to the sides).
  g.saveState();
  g.reduceClipRegion(area.expanded(1, 0));
  // Grid lines: snap steps, beats, bars.
  const int64_t s = snapTicks();
  const bool fine = xOfTick(s) - xOfTick(0) >= 4;  // a fine grid too dense to see: beats and bars only
  // Drums: every other beat a shade lighter, and an empty slot drawn in each step, so the steps read like
  // the pads of a drum machine; the lines below are then only beats and bars.
  const bool slots = drums() && xOfTick(s) - xOfTick(0) >= 6;
  if (drums()) {
    for (int64_t beat = doc::kPpq; beat < pattern()->lengthTicks; beat += 2 * doc::kPpq) {
      g.setColour(juce::Colours::white.withAlpha(0.025f));
      g.fillRect(xOfTick(beat), area.getY(), xOfTick(std::min(beat + doc::kPpq, pattern()->lengthTicks)) - xOfTick(beat), area.getHeight());
    }
    if (slots)
      for (size_t i = 0; i < r.size(); ++i)
        for (int64_t tick = 0; tick < pattern()->lengthTicks; tick += s) {
          doc::Note cell;
          cell.tick = tick;
          cell.pitch = r[i];
          cell.length = std::min(s, pattern()->lengthTicks - tick);
          g.setColour(juce::Colours::white.withAlpha(tick % doc::kPpq == 0 ? 0.11f : 0.065f));
          g.fillRoundedRectangle(noteBounds(cell).toFloat(), theme::kCorner);
        }
  }
  for (int64_t tick = 0; tick <= pattern()->lengthTicks; tick += fine && !slots ? s : doc::kPpq) {
    const bool bar = tick % (4 * doc::kPpq) == 0, beat = tick % doc::kPpq == 0;
    g.setColour(bar ? kBarLine : beat ? kGridLine.brighter(0.2f) : kGridLine);
    g.drawVerticalLine(xOfTick(tick), (float)area.getY(), (float)area.getBottom());
  }
  // Drums: how long each open hat rings, a thin line to the next hat that cuts it (the hats are one choke group;
  // the ride and the shaker are not in it). Without a next hat: to the end of the pattern.
  if (drums()) {
    const auto cuts = [](int p) { return p == kClosed || p == kOpen || p == kPedal; };
    for (const auto& n : pattern()->notes) {
      if (n.pitch != kOpen) continue;
      int64_t until = pattern()->lengthTicks;
      for (const auto& m : pattern()->notes)
        if (cuts(m.pitch) && m.tick > n.tick && m.tick < until) until = m.tick;
      const auto b = noteBounds(n);
      const int x1 = xOfTick(until) - 2;
      if (b.isEmpty() || x1 <= b.getRight() + 2) continue;
      g.setColour(kNote.withAlpha(0.55f));
      g.fillRect(b.getRight(), b.getCentreY() - 1, x1 - b.getRight(), 2);
      g.fillRect(x1 - 1, b.getCentreY() - 4, 2, 8);  // where it is cut
    }
  }
  // Notes: brighter = louder; selected ones lighter, with a white edge. Drum steps: a pad whose filled part
  // (from the bottom) is how loud it is.
  for (const auto& n : pattern()->notes) {
    const auto b = noteBounds(n);
    if (b.isEmpty()) continue;
    if (drums()) {
      const bool sel = selected_.count(n.id) > 0;
      const juce::Colour c = sel ? kSelected : kNote;
      const auto f = b.toFloat();
      g.setColour(c.withAlpha(0.28f));
      g.fillRoundedRectangle(f, theme::kCorner);
      const float h = std::max(3.0f, f.getHeight() * (float)std::clamp(n.velocity, 0.0, 1.0));
      g.setColour(c);
      g.fillRoundedRectangle(f.withTrimmedTop(f.getHeight() - h), theme::kCorner);
      g.setColour(sel ? juce::Colours::white : c);
      g.drawRoundedRectangle(f.reduced(0.5f), theme::kCorner, sel ? 1.5f : 1.0f);
      if (n.hasRoll && n.roll.count > 1) {
        g.setColour(kText);
        g.drawText("x" + juce::String(n.roll.count), b, juce::Justification::centred);
      }
      continue;
    }
    const juce::Colour c = selected_.count(n.id) ? kSelected : kNote;
    g.setColour(c.withAlpha(0.4f + 0.6f * (float)n.velocity));
    g.fillRoundedRectangle(b.toFloat(), 2);
    g.setColour(selected_.count(n.id) ? juce::Colours::white : c);
    g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 2, selected_.count(n.id) ? 1.5f : 1.0f);
    if (n.hasRoll && n.roll.count > 1) {
      g.setColour(kText);
      g.drawText("x" + juce::String(n.roll.count), b, juce::Justification::centred);
    }
  }
  // The ghost note and the guides (under the mouse, or of the note being moved).
  paintOverlay(g, drag_ == Drag::None ? overlay_ : dragOverlay());
  g.restoreState();
  // The select tool's box.
  if (!marquee_.isEmpty()) {
    g.setColour(theme::kAccent.withAlpha(0.15f));
    g.fillRect(marquee_);
    g.setColour(theme::kAccent);
    const float dashes[] = {4.0f, 3.0f};
    const auto m = marquee_.toFloat();
    for (auto line : {juce::Line<float>(m.getTopLeft(), m.getTopRight()), juce::Line<float>(m.getTopRight(), m.getBottomRight()),
                      juce::Line<float>(m.getBottomRight(), m.getBottomLeft()), juce::Line<float>(m.getBottomLeft(), m.getTopLeft())})
      g.drawDashedLine(line, dashes, 2, 1.2f);
  }
  // Velocity lane: a bar per note (its height the velocity), selected ones in the accent.
  {
    const auto lane = velocityLane();
    g.setColour(kGridDark);
    g.fillRect(lane);
    g.setColour(kGridLine);
    g.drawHorizontalLine(lane.getY() + lane.getHeight() / 2, (float)lane.getX(), (float)lane.getRight());
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
    g.drawText(lang::tr("VELOCITY"), lane.getX() - kLabels, lane.getCentreY() - 6, kLabels - 26, 12, juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText("127", lane.getX() - 26, lane.getY(), 22, 12, juce::Justification::centredRight);
    g.drawText("64", lane.getX() - 26, lane.getCentreY() - 6, 22, 12, juce::Justification::centredRight);
    g.drawText("0", lane.getX() - 26, lane.getBottom() - 12, 22, 12, juce::Justification::centredRight);
    g.saveState();
    g.reduceClipRegion(lane);
    for (const auto& n : pattern()->notes) {
      const int x0 = xOfTick(n.tick), w = juce::jlimit(3, drums() ? 14 : 8, xOfTick(n.tick + n.length) - x0 - 2);
      const int h = std::max(2, (int)std::round((lane.getHeight() - 4) * n.velocity));
      g.setColour(selected_.count(n.id) ? theme::kAccent : theme::kClipNote.withAlpha(0.55f));
      g.fillRect(x0, lane.getBottom() - h, w, h);
    }
    g.restoreState();
  }
  // Where the pattern is playing: the start line's own colour, as on the arrangement.
  g.setColour(theme::kStartLine);
  if (const int x = playheadX(); x >= 0) {
    g.fillRect(x, area.getY() - kRuler, 1, area.getHeight() + kRuler);
    juce::Path mark;
    mark.addTriangle((float)x - 4, (float)(area.getY() - kRuler), (float)x + 5, (float)(area.getY() - kRuler), (float)x + 0.5f, (float)(area.getY() - kRuler + 6));
    g.fillPath(mark);
  }
}

void PatternEditor::clickCell(int64_t tick, int pitch) {
  if (!currentTrack()) return;
  const std::string trackId = trackId_, patternId = patternId_;
  const int64_t length = std::min<int64_t>(pattern()->lengthTicks, drums() ? std::min<int64_t>(snapTicks(), 240) : snapTicks());
  bool added = false;
  std::string newId;
  host_.applyEdit([&](doc::Document& d) {
    doc::Pattern& p = doc::patternById(doc::trackById(d, trackId), patternId);
    newId = doc::newNoteId(p);
    added = doc::toggleStep(p, tick, pitch, length, 0.8);
  }, drums() ? u8("Step") : u8("Note"));
  selected_.clear();
  if (added && !drums()) selected_.insert(newId);
  revealSelection();
  repaint();
}

// The view (two octaves) moves so the selection's first note is in it; it stays put when that note already is.
void PatternEditor::revealSelection() {
  if (!currentTrack() || drums() || selected_.empty()) return;
  const doc::Note* first = nullptr;
  for (const auto& n : pattern()->notes)
    if (selected_.count(n.id) && (!first || n.tick < first->tick || (n.tick == first->tick && n.pitch < first->pitch))) first = &n;
  if (!first || (first->pitch >= lowPitch_ && first->pitch < lowPitch_ + keysShown())) return;
  lowPitch_ = std::clamp(first->pitch - keysShown() / 2, 0, 128 - keysShown());
  repaint();
}

void PatternEditor::selectAll() {
  selected_.clear();
  if (const doc::Track* t = currentTrack()) for (const auto& n : pattern()->notes) selected_.insert(n.id);
  revealSelection();
  repaint();
}

void PatternEditor::duplicateSelection() {
  if (selected_.empty() || !currentTrack()) return;
  const std::string trackId = trackId_, patternId = patternId_;
  const std::vector<std::string> ids(selected_.begin(), selected_.end());
  std::vector<std::string> copies;
  const int64_t s = snapTicks();
  host_.applyEdit([&](doc::Document& d) { copies = doc::duplicateNotes(doc::patternById(doc::trackById(d, trackId), patternId), ids, s); }, "Duplicate notes");
  selected_ = std::set<std::string>(copies.begin(), copies.end());
  repaint();
}

void PatternEditor::deleteSelection() {
  if (selected_.empty() || !currentTrack()) return;
  const std::string trackId = trackId_, patternId = patternId_;
  const std::set<std::string> ids = selected_;
  host_.applyEdit([&](doc::Document& d) {
    auto& notes = doc::patternById(doc::trackById(d, trackId), patternId).notes;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const doc::Note& n) { return ids.count(n.id) > 0; }), notes.end());
  }, "Delete notes");
  selected_.clear();
  repaint();
}

void PatternEditor::mouseDown(const juce::MouseEvent& e) {
  grabKeyboardFocus();
  if (!overlay_.area().isEmpty()) repaint(overlay_.area().expanded(2));
  overlay_ = {};  // the click acts; the ghost comes back when the mouse moves
  const doc::Track* t = currentTrack();
  if (!t) return;
  // The ruler (bar.beat over the notes): a click or a drag moves the start line, to the grid (Diogo 2026-11-09:
  // "não consigo mexer no ponto iniciar").
  if (const auto g = grid(); e.y >= g.getY() - kRuler && e.y < g.getY() && e.x >= g.getX() && e.x <= g.getRight()) {
    drag_ = Drag::Playhead;
    movePlayheadTo(e.x);
    return;
  }
  if (velocityLane().contains(e.getPosition())) {
    drag_ = Drag::Velocity;
    host_.startGesture(u8("Velocity"));
    setVelocityAt(e.x, e.y);
    return;
  }
  if (drums() && hoverPitch_ >= 0 && removableRow(hoverPitch_) && removeMark(hoverPitch_).contains(e.getPosition())) {
    removeAddedRow(hoverPitch_);
    return;
  }
  if (!grid().contains(e.getPosition())) return;
  bool edge = false;
  const doc::Note* hit = noteAt(e.getPosition(), &edge);
  // Deleting is the same everywhere: the erase tool, or a right-click (drag to erase more) with any tool.
  // (No double-click: two quick clicks to place and move a note must never delete it.)
  if (tool_ == Tool::Erase || e.mods.isPopupMenu()) {
    drag_ = Drag::Erase;
    host_.startGesture(u8("Erase notes"));
    eraseAt(e.getPosition());
    return;
  }
  if (tool_ == Tool::Select && (!hit || drums())) {
    if (hit) {  // drums: a click selects the step (the draw tool turns it off)
      if (!e.mods.isCtrlDown()) selected_.clear();
      selected_.insert(hit->id);
      repaint();
      return;
    }
    drag_ = Drag::Marquee;
    marqueeStart_ = e.getPosition();
    marqueeBase_ = e.mods.isCtrlDown() ? selected_ : std::set<std::string>();
    selected_ = marqueeBase_;
    marquee_ = {};
    repaint();
    return;
  }
  if (drums()) return stepMouseDown(e, hit);
  if (!hit) {
    if (!e.mods.isCtrlDown()) selected_.clear();
    clickCell(tickAtX(e.x, true), pitchAtY(e.y));
    return;
  }
  if (e.mods.isCtrlDown()) {
    if (!selected_.erase(hit->id)) selected_.insert(hit->id);
    repaint();
    return;
  }
  if (!selected_.count(hit->id)) selected_ = {hit->id};
  drag_ = edge ? Drag::Resize : Drag::Move;
  dragNote_ = hit->id;
  dragStartTick_ = tickAtX(e.x, false);
  dragOffsetTicks_ = dragStartTick_ - hit->tick;
  dragStartPitch_ = hit->pitch;
  dragOriginal_.clear();
  for (const auto& n : pattern()->notes) if (selected_.count(n.id)) dragOriginal_.push_back(n);
  host_.startGesture(edge ? u8("Length") : u8("Move notes"));
  repaint();
}

// The step grid: a click on an empty cell turns its step on; a click on a step (or on the cell of a step
// moved off the grid) turns it off; a drag moves the step (Shift: anywhere, off the grid).
void PatternEditor::stepMouseDown(const juce::MouseEvent& e, const doc::Note* hit) {
  if (!hit) {
    const int64_t cell = tickAtX(e.x, true), s = snapTicks();
    const int pitch = pitchAtY(e.y);
    for (const auto& n : pattern()->notes)
      if (n.pitch == pitch && n.tick >= cell && n.tick < cell + s) { hit = &n; break; }
    if (!hit) { clickCell(cell, pitch); return; }
  }
  gestureOpen_ = false;
  selected_ = {hit->id};
  drag_ = Drag::Move;
  dragNote_ = hit->id;
  dragStartTick_ = tickAtX(e.x, false);
  dragOffsetTicks_ = dragStartTick_ - hit->tick;
  dragStartPitch_ = hit->pitch;
  dragOriginal_ = {*hit};
  repaint();
}

void PatternEditor::mouseDrag(const juce::MouseEvent& e) {
  const doc::Track* t = currentTrack();
  if (!t || drag_ == Drag::None) return;
  if (drag_ == Drag::Velocity) { setVelocityAt(e.x, e.y); return; }
  if (drag_ == Drag::Playhead) { movePlayheadTo(e.x); return; }
  if (drag_ == Drag::Erase) { eraseAt(e.getPosition()); return; }
  if (drag_ == Drag::Marquee) {
    marquee_ = juce::Rectangle<int>(marqueeStart_, e.getPosition()).getIntersection(grid());
    selected_ = marqueeBase_;
    for (const auto& n : pattern()->notes)
      if (noteBounds(n).intersects(marquee_)) selected_.insert(n.id);
    repaint();
    return;
  }
  if (drums()) {
    if (!gestureOpen_ && e.getDistanceFromDragStart() < 4) return;  // still a click
    if (!gestureOpen_) { host_.startGesture(u8("Move notes")); gestureOpen_ = true; }
  }
  const bool free = drag_ == Drag::Move && e.mods.isShiftDown();  // Shift: moves off the grid (lengths stay on it)
  const int64_t s = free ? 1 : snapTicks(), len = pattern()->lengthTicks;
  const std::string trackId = trackId_, patternId = patternId_;
  if (drag_ == Drag::Move) {
    // The grabbed note snaps to the grid; the others keep their distance to it.
    const auto grabbed = std::find_if(dragOriginal_.begin(), dragOriginal_.end(), [&](const doc::Note& n) { return n.id == dragNote_; });
    if (grabbed == dragOriginal_.end()) return;
    const int64_t wanted = (tickAtX(e.x, false) - dragOffsetTicks_ + s / 2) / s * s;
    const int64_t dt = wanted - grabbed->tick;
    const int dp = pitchAtY(e.y) - dragStartPitch_;
    const auto originals = dragOriginal_;
    host_.applyEdit([&](doc::Document& d) {
      for (auto& n : doc::patternById(doc::trackById(d, trackId), patternId).notes)
        for (const auto& o : originals)
          if (o.id == n.id) {
            n.tick = std::clamp<int64_t>(o.tick + dt, 0, len - o.length);
            n.pitch = std::clamp(o.pitch + dp, 0, 127);
          }
    }, u8("Move notes"));
  } else {
    const std::string id = dragNote_;
    const int64_t end = std::max<int64_t>((tickAtX(e.x, false) + s / 2) / s * s, 1);
    host_.applyEdit([&](doc::Document& d) {
      for (auto& n : doc::patternById(doc::trackById(d, trackId), patternId).notes)
        if (n.id == id) n.length = std::clamp<int64_t>(end - n.tick, std::min<int64_t>(s, len - n.tick), len - n.tick);
    }, u8("Length"));
  }
}

void PatternEditor::mouseMove(const juce::MouseEvent& e) { updateHover(e.getPosition()); }

void PatternEditor::mouseExit(const juce::MouseEvent&) { updateHover({-1, -1}); }

void PatternEditor::mouseUp(const juce::MouseEvent& e) {
  if (drag_ == Drag::None) return;
  if (const auto dragged = dragOverlay().area(); !dragged.isEmpty()) repaint(dragged.expanded(2));  // the moved note's guides go
  struct After { PatternEditor& ed; juce::Point<int> p; ~After() { ed.updateHover(p); } } after{*this, e.getPosition()};  // then the ghost, if any
  const Drag ended = drag_;
  drag_ = Drag::None;
  if (ended == Drag::Velocity || ended == Drag::Erase) { host_.finishGesture(); repaint(); return; }
  if (ended == Drag::Marquee) { marquee_ = {}; repaint(); return; }
  if (ended == Drag::Playhead) return;
  if (!drums()) { host_.finishGesture(); return; }
  // Where the step ended: the same place counts as a click (it turns off); on another step, the moved
  // one replaces it (two steps never share a cell).
  const doc::Note* moved = nullptr;
  for (const auto& n : pattern()->notes) if (n.id == dragNote_) moved = &n;
  const doc::Note original = dragOriginal_.empty() ? doc::Note{} : dragOriginal_.front();
  const bool inPlace = moved && moved->tick == original.tick && moved->pitch == original.pitch;
  if (moved && !inPlace) {
    const std::string trackId = trackId_, patternId = patternId_, id = dragNote_;
    const int64_t tick = moved->tick;
    const int pitch = moved->pitch;
    host_.applyEdit([&](doc::Document& d) {
      auto& notes = doc::patternById(doc::trackById(d, trackId), patternId).notes;
      notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const doc::Note& n) { return n.id != id && n.tick == tick && n.pitch == pitch; }), notes.end());
    }, u8("Move notes"));
  }
  if (gestureOpen_) host_.finishGesture();
  gestureOpen_ = false;
  if (inPlace) clickCell(original.tick, original.pitch);  // a click (or a drag that came back): off
  {
    selected_.clear();
    repaint();
  }
}

// The notes an Edit tool works on: those that start in the bars shown, only the selected ones when some are.
std::set<std::string> PatternEditor::editIds() const {
  std::set<std::string> ids;
  if (!currentTrack()) return ids;
  const auto shown = visibleBars();
  for (const auto& n : pattern()->notes)
    if (n.tick >= shown.first && n.tick < shown.second && (selected_.empty() || selected_.count(n.id))) ids.insert(n.id);
  return ids;
}

// One Edit tool: one undo step on the notes of editIds(); the status line says what changed.
void PatternEditor::editNotes(const juce::String& label, const std::function<int(doc::Pattern&, const std::set<std::string>&)>& op) {
  if (!currentTrack()) return;
  const std::set<std::string> ids = editIds();
  if (ids.empty()) { say(label + u8(": no notes in the bars shown.")); return; }
  const std::string trackId = trackId_, patternId = patternId_;
  int changed = 0;
  try {
    doc::Pattern trial = *pattern();  // nothing to change: no empty undo step
    changed = op(trial, ids);
  } catch (const std::exception& e) {  // refused (too many notes...): said, nothing changed
    say(label + u8(": ") + juce::String::fromUTF8(e.what()));
    return;
  }
  if (changed == 0) { say(label + u8(": nothing to change.")); return; }
  host_.applyEdit([&](doc::Document& d) { op(doc::patternById(doc::trackById(d, trackId), patternId), ids); }, label);
  documentChanged();  // some tools add or remove notes: the selection follows
  say(label + ": " + juce::String(changed) + (changed == 1 ? " note" : " notes") + u8(" (Ctrl+Z undoes)."));
}

void PatternEditor::editNotesRandom(const juce::String& label,
                                    const std::function<int(doc::Pattern&, const std::set<std::string>&, uint64_t)>& op) {
  if (!currentTrack()) return;
  if (editIds().empty()) { say(label + u8(": no notes in the bars shown.")); return; }
  // A draw may land on what is already there (one note in three, a pitch drawn equal to the old one): the next
  // seeds are tried, so a click changes something whenever a change is possible.
  // Never the same twice in a row of clicks: the states this action has already made on this pattern are skipped
  // (the list starts again when the notes were changed some other way).
  auto keyOf = [](const doc::Pattern& p) {
    std::string k;
    for (const auto& n : p.notes) k += std::to_string(n.tick) + ',' + std::to_string(n.length) + ',' + std::to_string(n.pitch) + ',' + std::to_string(std::lround(n.velocity * 1000)) + ';';
    return k;
  };
  auto& seen = randomSeen_[trackId_ + "|" + patternId_ + "|" + label.toStdString()];
  const std::string now = keyOf(*pattern());
  if (!seen.count(now)) seen = {now};
  uint64_t seed = grooveSeed_++, firstChange = 0;
  bool fresh = false;
  for (int tries = 0; tries < 48; ++tries, seed = grooveSeed_++) {
    doc::Pattern trial = *pattern();
    if (op(trial, editIds(), seed) <= 0) continue;
    if (firstChange == 0) firstChange = seed;
    if (!seen.count(keyOf(trial))) { fresh = true; break; }
  }
  if (!fresh && firstChange != 0) seed = firstChange;
  editNotes(label, [=](doc::Pattern& p, const std::set<std::string>& ids) { return op(p, ids, seed); });
  if (const doc::Pattern* after = pattern()) seen.insert(keyOf(*after));
  if (!fresh && firstChange != 0) say(label + u8(": every way these notes can change has been shown; this one repeats an earlier one."));
}

void PatternEditor::say(const juce::String& text) {
  if (onMessage) onMessage(text);
  if (text.isEmpty() || getWidth() <= 0) return;
  const juce::Font font{juce::FontOptions(theme::kFontBody)};
  const int w = std::min(getWidth() - 16, (int)std::ceil(juce::GlyphArrangement::getStringWidth(font, text)) + 24);
  const int lines = std::max(1, (int)std::ceil(juce::GlyphArrangement::getStringWidth(font, text) / (float)std::max(1, w - 24)));
  toast_.setFont(font);
  toast_.setText(text, juce::dontSendNotification);
  toast_.setBounds(std::max(8, edit_.getRight() - w), edit_.getBottom() + 4, w, 10 + 17 * std::min(lines, 3));
  toast_.setVisible(true);
  toast_.toFront(false);
  const int stamp = ++toastStamp_;
  juce::Timer::callAfterDelay(4500, [safe = juce::Component::SafePointer<PatternEditor>(this), stamp] {
    if (safe && safe->toastStamp_ == stamp) safe->toast_.setVisible(false);
  });
}

void PatternEditor::quantize() {
  if (!currentTrack()) return;
  const int64_t grid = snapTicks();
  const double strength = quantizeStrength, sw = swing;
  // Nothing moves: say why, so it does not look broken (notes already on a fine grid such as 1/64, or 0 % strength).
  const std::set<std::string> ids = editIds();
  if (!ids.empty()) {
    doc::Pattern trial = *pattern();
    if (doc::quantizeNotes(trial, ids, grid, strength, sw) == 0) {
      juce::String snapName;
      for (int id : kSnapOrder) if (id == snap_.getSelectedId()) snapName = kSnapNames[id - 1];
      say(strength <= 0 ? u8("Quantize: the strength is 0 %, so nothing moves. Raise Strength first.")
                        : u8("Quantize: the notes are already on the ") + snapName +
                              u8(" grid, so nothing moves. Choose a bigger grid (1/16 or 1/8) to pull them onto it. "
                                 "To change the pattern itself use Edit > Patterns or Library > Grooves."));
      return;
    }
  }
  editNotes(u8("Quantize"), [=](doc::Pattern& p, const std::set<std::string>& ids) { return doc::quantizeNotes(p, ids, grid, strength, sw); });
}

void PatternEditor::humanize() {
  const int64_t timing = std::max<int64_t>(1, snapTicks() / 8);
  const uint64_t seed = grooveSeed_++;
  editNotes(u8("Humanize"), [=](doc::Pattern& p, const std::set<std::string>& ids) { return doc::humanizeNotes(p, ids, timing, 0.08, seed); });
}

// ---------------------------------------------------------------- Tools: create, not only trim

std::pair<int64_t, int64_t> PatternEditor::visibleBars() const {
  if (!currentTrack()) return {0, 0};
  const int64_t bar = 4 * doc::kPpq, len = pattern()->lengthTicks;
  const int64_t from = std::clamp<int64_t>((int64_t)std::floor(shownStart() / (double)bar) * bar, 0, len);
  const int64_t to = std::clamp<int64_t>((int64_t)std::ceil((shownStart() + shownLength()) / (double)bar - 1e-9) * bar, from, len);
  return {from, to};
}

std::set<std::string> PatternEditor::toolIds() const { return selected_; }

void PatternEditor::normalize(const doc::NormalizeOptions& options, const juce::String& label) {
  if (!currentTrack()) return;
  const auto shown = visibleBars();
  const int64_t from = shown.first, to = shown.second;
  const std::string trackId = trackId_, patternId = patternId_;
  const doc::NoteScope scope{from, to, toolIds()};
  doc::NormalizeOptions o = options;
  o.grid = snapTicks();
  int changed = 0;
  {
    doc::Pattern trial = *pattern();
    changed = doc::normalizeNotes(trial, scope, o);
  }
  if (changed == 0) { say(label + u8(": nothing to change in the bars shown.")); return; }
  host_.applyEdit([&](doc::Document& d) { doc::normalizeNotes(doc::patternById(doc::trackById(d, trackId), patternId), scope, o); }, label);
  say(label + ": " + juce::String(changed) + (changed == 1 ? " note" : " notes") + u8(" changed (Ctrl+Z undoes)."));
}

std::string PatternEditor::followKickTrack() const {
  if (!followKickChoice_.empty())
    if (const doc::Track* c = host_.currentDocument().findTrack(followKickChoice_); c && doc::playsPatterns(*c)) return c->id;
  const doc::Track* t = currentTrack();
  if (t && t->instrument.kind == "builtin" && t->instrument.name == "kick") return t->id;
  return doc::findKickTrack(host_.currentDocument());
}

std::string PatternEditor::followBassTrack() const {
  const std::string kick = followKickTrack();
  if (!followBassChoice_.empty())
    if (const doc::Track* c = host_.currentDocument().findTrack(followBassChoice_); c && doc::playsPatterns(*c) && c->id != kick) return c->id;
  const doc::Track* t = currentTrack();
  if (t && t->id != kick && doc::playsPatterns(*t) && !drums() && doc::findBassTrack(host_.currentDocument()) == t->id) return t->id;
  return doc::findBassTrack(host_.currentDocument());
}

// FOLLOW: what 0 zone, 1 kick, 2 bass, 3 on/off. One undo step each; the editor stays on the bars it showed.
void PatternEditor::follow(int what) {
  const doc::Track* t = currentTrack();
  if (!t) return;
  const std::string trackId = trackId_, patternId = patternId_, kick = followKickTrack(), bass = followBassTrack();
  const auto shown = visibleBars();
  const doc::Document& now = host_.currentDocument();
  const auto ranges = doc::songRangesOfPattern(now, trackId, patternId, shown.first, shown.second);
  doc::FollowSettings s;
  s.offsetTicks = followOffset_;
  s.looseTicks = followLoose_;
  s.seed = ++followSeed_;
  juce::String label, done;
  std::function<void(doc::Document&)> change;
  int64_t firstTick = ranges.empty() ? -1 : ranges.front().first;
  if (what == 3) {
    doc::FollowLink link;
    const bool on = doc::followLink(*t);
    if (on) {
      change = [trackId](doc::Document& d) { doc::removeFollowLink(d, trackId); };
      label = u8("FOLLOW off");
      done = u8("FOLLOW is off: ") + u8(t->name) + u8(" keeps the notes it has.");
    } else {
      link.mode = trackId == kick ? "stutter" : "kick";
      if (link.mode == "kick" && !followKickChoice_.empty()) link.source = kick;
      link.settings = s;
      change = [trackId, link](doc::Document& d) { doc::setFollowLink(d, trackId, link); };
      label = u8("FOLLOW on");
      done = u8("FOLLOW is on: ") + u8(t->name) + (link.mode == "stutter" ? u8(" plays every STUTTER of the song, at every change.")
                                                                         : u8(" follows the kick wherever the song has a STUTTER, at every change."));
    }
  } else if (what == 0) {
    const auto onsets = doc::songOnsetsOfNotes(now, trackId, patternId, selected_);
    if (onsets.empty()) { say(u8("FOLLOW: the selected notes are not played by any clip of the song.")); return; }
    firstTick = onsets.front();
    const auto zones = doc::followZones(onsets);
    change = [kick, bass, zones, s](doc::Document& d) {
      doc::kickFollow(d, kick, zones, s);
      if (!bass.empty() && bass != kick) doc::bassFollowZones(d, bass, kick, zones, s);
    };
    label = u8("Kick and bass follow the stutter");
    done = label + u8(": ") + juce::String((int)onsets.size()) + u8(" notes in ") + juce::String((int)zones.size()) +
           (zones.size() == 1 ? u8(" zone") : u8(" zones")) + u8(" (Ctrl+Z undoes).");
  } else {
    if (ranges.empty()) { say(u8("FOLLOW: no clip of the song plays these bars.")); return; }
    if (what == 1) {
      std::vector<int64_t> onsets;
      for (const auto& [a, b] : ranges) {
        const auto v = doc::stutterOnsets(now, kick, a, b);
        onsets.insert(onsets.end(), v.begin(), v.end());
      }
      if (onsets.empty()) { say(u8("FOLLOW: no STUTTER notes in the bars shown. Select the stutter's notes and use 'Kick and bass follow the selected notes'.")); return; }
      const auto zones = doc::followZones(onsets);
      change = [kick, zones, s](doc::Document& d) { doc::kickFollow(d, kick, zones, s); };
      label = u8("Kick follows the stutter");
      done = label + u8(": ") + juce::String((int)onsets.size()) + u8(" kicks (Ctrl+Z undoes).");
    } else {
      change = [bass, kick, ranges, s](doc::Document& d) { for (const auto& [a, b] : ranges) doc::bassFollow(d, bass, kick, a, b, s); };
      label = u8("Bass follows the kick");
      done = label + u8(" in the bars shown (Ctrl+Z undoes).");
    }
  }
  {  // a trial first: an error is said, nothing changes
    doc::Document trial = now;
    try { change(trial); } catch (const std::exception& e) { say(u8("FOLLOW: ") + u8(e.what())); return; }
    if (doc::sameContent(trial, now)) { say(u8("FOLLOW: nothing to change.")); return; }
  }
  host_.applyEdit(change, label);
  // The bars followed may now play a pattern of their own: show it, where the song plays them.
  if (firstTick >= 0) {
    const doc::Document& d = host_.currentDocument();
    if (const doc::Track* tr = d.findTrack(trackId); tr && tr->clipsUsing(patternId) == 0)
      for (const auto& c : tr->clips)
        if (firstTick >= c.startTick && firstTick < c.startTick + c.lengthTicks) { setPattern(c.patternId); break; }
  }
  documentChanged();
  say(done);
}

void PatternEditor::density(const std::string& mode, double amount) {
  const doc::Track* t = currentTrack();
  if (!t) return;
  const auto shown = visibleBars();
  const int64_t from = shown.first, to = shown.second;
  const std::string trackId = trackId_, patternId = patternId_;
  const doc::NoteScope scope{from, to, toolIds()};
  doc::DensityOptions o;
  o.mode = mode;
  o.amount = amount;
  o.how = densityHow;
  o.grid = snapTicks();
  o.seed = grooveSeed_++;
  o.pitch = drums() ? rows().back() : doc::trackRoot(*t, pattern());
  doc::DensityResult r;
  {
    doc::Pattern trial = *pattern();
    r = doc::changeDensity(trial, scope, o);
  }
  const juce::String label = amount >= 0 ? u8("More notes") : u8("Fewer notes");
  if (r.added == 0 && r.removed == 0) {
    say(label + ": " + (r.note.empty() ? u8("nothing to change in the bars shown.") : u8(r.note) + "."));
    return;
  }
  host_.applyEdit([&](doc::Document& d) { doc::changeDensity(doc::patternById(doc::trackById(d, trackId), patternId), scope, o); }, label);
  documentChanged();
  say(label + ": " + juce::String(r.before) + u8(" → ") + juce::String(r.after) + " notes" +
              (r.after != r.target ? u8(" (asked ") + juce::String(r.target) + ")" : juce::String()) +
      (r.note.empty() ? juce::String() : u8(" · ") + u8(r.note)) + u8(". Again: another take; Ctrl+Z undoes."));
}

void PatternEditor::buildup(int bars, bool roll, bool gap) {
  const doc::Track* t = currentTrack();
  if (!t) return;
  const auto shown = visibleBars();
  const int64_t from = shown.first, to = shown.second;
  const int64_t length = std::min<int64_t>((int64_t)bars * 4 * doc::kPpq, to - from);
  if (length < doc::kPpq) return;
  doc::BuildupOptions o;
  o.start = to - length;
  o.length = length;
  o.roll = roll;
  o.gapTicks = gap ? doc::kPpq : 0;
  const int root = doc::trackRoot(*t, pattern());
  o.pitch = drums() ? rows().front() : root >= 0 ? root : 38;
  const std::string trackId = trackId_, patternId = patternId_;
  int written = 0;
  host_.applyEdit([&](doc::Document& d) { written = doc::makeBuildup(doc::patternById(doc::trackById(d, trackId), patternId), o); },
                  u8("Build-up"));
  documentChanged();
  say(u8("Build-up: ") + juce::String(written) + u8(" notes over ") + juce::String(length / (4 * doc::kPpq)) +
                           (length >= 8 * doc::kPpq ? " bars" : " bar") + (roll ? u8(", roll at the end") : juce::String()) +
                           (gap ? u8(", one beat of silence before the drop.") : juce::String(".")));
}

void PatternEditor::applyPreset(const doc::PatternPreset& preset, int bars, bool add) {
  const doc::Track* t = currentTrack();
  if (!t) { say(u8("Select a note track first: the pattern goes on it.")); return; }
  const auto shown = visibleBars();
  const int64_t from = shown.first, to = shown.second;
  (void)to;
  const int64_t length = (int64_t)std::clamp(bars, 1, 64) * 4 * doc::kPpq;
  const int root = doc::trackRoot(*t, pattern());
  const std::string trackId = trackId_, patternId = patternId_;
  int written = 0;
  host_.applyEdit([&](doc::Document& d) {
    doc::Pattern& p = doc::patternById(doc::trackById(d, trackId), patternId);
    if (from + length > p.lengthTicks) doc::extendPatternRepeating(p, std::min<int64_t>(from + length, 64LL * 4 * doc::kPpq));
    written = doc::applyPreset(p, preset, from, length, add ? "add" : "replace", root);
  }, u8("Pattern: ") + u8(preset.name));
  documentChanged();
  say(u8(preset.name) + ": " + juce::String(written) + u8(" notes from bar ") + juce::String(from / (4 * doc::kPpq) + 1) +
                           ", " + juce::String(bars) + (bars == 1 ? " bar" : " bars") + u8(" (Ctrl+Z undoes)."));
}

// Edit ▾ (Diogo, 2026-11-05: "one Edit menu, dropdowns that lead to other dropdowns"). Short names, one level per
// kind of change; every item is one undo step on the bars shown (the selected notes among them).
void PatternEditor::showTools() {
  if (!currentTrack()) return;
  const auto shown = visibleBars();
  const int64_t from = shown.first, to = shown.second;
  const int64_t bar = 4 * doc::kPpq;
  const bool drum = drums();
  juce::PopupMenu m;
  const juce::String where = "Bars " + juce::String(from / bar + 1) + (to - from > bar ? u8("–") + juce::String(to / bar) : juce::String()) +
                             (selected_.empty() ? juce::String() : u8(" · selected notes only"));
  m.addSectionHeader(where);
  enum { kNormEqual = 1, kNormAccent, kNormLength, kNormAlign, kNormAll, kMore = 100, kFewer = 200, kPerBeat = 300, kHow = 400,
         kBuild = 500, kBuildSec = 520, kBuildTrack = 560, kBuildRoll = 590, kBuildGap, kVary = 600, kSave = 700, kCustom = 710,
         kPCore = 900, kBuildupWindow = 901, kQuantize = 800, kHumanize = 801, kSwing = 1000, kTranspose = 1100, kChord = 1200,
         kLength = 1300, kLouder = 1400, kSofter, kShuffle = 1500, kRandomPitch, kHumanizeMore, kStutter = 1600, kAttach = 1700, kAttachPlan = 1701,
         kStrength = 1800, kPreview = 1950, kFollowZone = 2000, kFollowKick, kFollowBass, kFollowOn, kFollowOffset = 2100, kFollowLoose = 2200, kFollowKickPick = 2300, kFollowBassPick = 2500,
         kSuck = 3000, kPumShown = 3100, kPumSong = 3400, kBuildFx = 3700 };
  m.addItem(juce::PopupMenu::Item(u8("Play the selected notes")).setID(kPreview).setEnabled(!selected_.empty()));
  m.addSeparator();

  // Timing: where the notes sit.
  juce::PopupMenu timing;
  juce::String snapName;
  for (int id : kSnapOrder) if (id == snap_.getSelectedId()) snapName = kSnapNames[id - 1];
  timing.addItem(kQuantize, u8("Quantize (to ") + snapName + ", " + juce::String(std::lround(quantizeStrength * 100)) + "%)");
  // Quantize's strength and the selected notes' preview live here (Diogo 2026-11-09: they act on notes, not on the
  // track, so they left the Inspector).
  juce::PopupMenu strengthMenu;
  for (const int pct : {25, 50, 75, 100})
    strengthMenu.addItem(juce::PopupMenu::Item(juce::String(pct) + "%").setID(kStrength + pct).setTicked(std::lround(quantizeStrength * 100) == pct));
  timing.addSubMenu(u8("Strength (for Quantize)"), strengthMenu);
  juce::PopupMenu swingMenu;
  for (int pct : {0, 12, 25, 33, 50, 66})
    swingMenu.addItem(juce::PopupMenu::Item(juce::String(pct) + "%" + (pct == 33 ? u8("  (about a triplet)") : juce::String()))
                          .setID(kSwing + pct).setTicked(std::lround(swing * 100) == pct));
  timing.addSubMenu(u8("Swing (for Quantize)"), swingMenu);
  timing.addItem(kHumanize, u8("Humanize (small random timing and velocity)"));
  m.addSubMenu("Timing", timing);

  // Notes: pitch, chords, length, velocity.
  juce::PopupMenu notes;
  if (!drum) {
    juce::PopupMenu transpose;
    static const std::pair<int, const char*> steps[] = {{1, "+1 semitone"}, {-1, "−1 semitone"}, {12, "+1 octave"}, {-12, "−1 octave"},
                                                        {7, "+7 (a fifth up)"}, {-7, "−7 (a fifth down)"}};
    for (const auto& [st, name] : steps)
      transpose.addItem(kTranspose + 24 + st, u8(name));
    notes.addSubMenu("Transpose", transpose);
    juce::PopupMenu chord;
    chord.addItem(kChord + 1, u8("Octave above"));
    chord.addItem(kChord + 2, u8("Fifth above"));
    chord.addItem(kChord + 3, u8("Minor chord (3 notes)"));
    chord.addItem(kChord + 4, u8("Major chord (3 notes)"));
    notes.addSubMenu(u8("Add notes above"), chord);
  }
  juce::PopupMenu length;
  length.addItem(kLength + 1, u8("Legato (each until the next note)"));
  length.addItem(kLength + 2, u8("Half as long"));
  length.addItem(kLength + 3, u8("Twice as long"));
  length.addItem(kNormLength, u8("90% of the grid"));
  notes.addSubMenu("Length", length);
  juce::PopupMenu velocity;
  velocity.addItem(kLouder, u8("Louder (+15%)"));
  velocity.addItem(kSofter, u8("Softer (−15%)"));
  velocity.addItem(kNormEqual, u8("All equal (their average)"));
  velocity.addItem(kNormAccent, u8("Accents in every beat (KBBB 1 · .72 · .52 · .64)"));
  notes.addSubMenu("Velocity", velocity);
  notes.addSeparator();
  notes.addItem(kNormAll, u8("Clean up (equal velocity, 90% length, on the grid)"));
  m.addSubMenu("Notes", notes);

  // More and fewer notes (P-Core's density).
  juce::PopupMenu more;
  for (int pct : {10, 20, 30, 40}) more.addItem(kMore + pct, "+" + juce::String(pct) + "%");
  more.addItem(kCustom, u8("Other amount…"));
  more.addSeparator();
  juce::PopupMenu perBeat;
  for (int k : {1, 2, 3, 4, 6, 8}) perBeat.addItem(kPerBeat + k, juce::String(k) + (k == 1 ? " note" : " notes") + " per beat");
  more.addSubMenu("Notes per beat", perBeat);
  more.addSeparator();
  more.addSectionHeader("New notes are");
  const char* hows[] = {"fill", "ghost", "ratchet", "octave"};
  const char* howNames[] = {"Fills of the empty steps", "Ghost notes (soft, short)", "Ratchets (a note repeats 2-4 times)", "Octave copies"};
  for (int i = 0; i < 4; ++i) more.addItem(juce::PopupMenu::Item(u8(howNames[i])).setID(kHow + i).setTicked(densityHow == hows[i]));
  m.addSubMenu("More notes", more);
  juce::PopupMenu fewer;
  for (int pct : {10, 20, 30, 40}) fewer.addItem(kFewer + pct, u8("−") + juce::String(pct) + "%");
  m.addSubMenu("Fewer notes", fewer);

  juce::PopupMenu vary;
  vary.addItem(kVary + 1, u8("Octave jumps (some notes)"));
  vary.addItem(kVary + 2, u8("Reverse"));
  vary.addItem(kVary + 3, u8("Thin out (about one in four)"));
  vary.addItem(kVary + 4, u8("Accents on the beat"));
  m.addSubMenu("Variation", vary);

  // STUTTER (note slice): each note becomes repeats inside its own length, as ordinary notes.
  juce::PopupMenu stutter;
  stutter.addSectionHeader(u8("Each note becomes repeats inside its length"));
  for (size_t i = 0; i < doc::stutterPresets().size(); ++i) {
    const auto& p = doc::stutterPresets()[i];
    stutter.addItem(kStutter + 1 + (int)i, u8(p.name) + u8("  · ") + u8(p.style));
  }
  m.addSubMenu("STUTTER", stutter);

  // FOLLOW (Diogo, 2026-11-09): the kick plays the stutter's onsets, the bass fills the steps after each kick. Once on a
  // zone (the selected stutter notes, or the bars shown), or on: the track keeps following at every edit.
  {
    const doc::Document& d = host_.currentDocument();
    const std::string kick = followKickTrack(), bass = followBassTrack();
    const doc::Track* here = currentTrack();
    auto name = [&](const std::string& id) { const doc::Track* t = d.findTrack(id); return t ? u8(t->name) : u8("(none)"); };
    juce::PopupMenu follow;
    follow.addSectionHeader(u8("Kick: ") + name(kick) + u8(" · Bass: ") + name(bass));
    follow.addItem(juce::PopupMenu::Item(u8("Kick and bass follow the selected notes (a stutter zone)"))
                       .setID(kFollowZone).setEnabled(!selected_.empty() && !kick.empty()));
    follow.addItem(juce::PopupMenu::Item(u8("Kick follows the STUTTER (bars shown)")).setID(kFollowKick).setEnabled(!kick.empty()));
    follow.addItem(juce::PopupMenu::Item(u8("Bass follows the kick (bars shown)")).setID(kFollowBass).setEnabled(!kick.empty() && !bass.empty() && kick != bass));
    follow.addSeparator();
    juce::PopupMenu offset, loose;
    static const int64_t offsets[] = {-120, -60, -30, 0, 30, 60, 120};
    static const char* offsetNames[] = {"1/32 early", "1/64 early", "1/128 early", "Exact (on the stutter)", "1/128 late", "1/64 late", "1/32 late"};
    for (int i = 0; i < 7; ++i) offset.addItem(juce::PopupMenu::Item(u8(offsetNames[i])).setID(kFollowOffset + i).setTicked(followOffset_ == offsets[i]));
    static const int64_t looses[] = {0, 15, 30, 60};
    static const char* looseNames[] = {"Tight (100% on time)", "A little (up to 1/256)", "Loose (up to 1/128)", "Very loose (up to 1/64)"};
    for (int i = 0; i < 4; ++i) loose.addItem(juce::PopupMenu::Item(u8(looseNames[i])).setID(kFollowLoose + i).setTicked(followLoose_ == looses[i]));
    // Which tracks follow (Diogo, 2026-11-09): automatic by name, or chosen here; kept while Core is open.
    juce::PopupMenu kickPick, bassPick;
    kickPick.addItem(juce::PopupMenu::Item(u8("Automatic (the kick instrument, or a track named kick)")).setID(kFollowKickPick).setTicked(followKickChoice_.empty()));
    bassPick.addItem(juce::PopupMenu::Item(u8("Automatic (a track named bass, kbbb or baixo)")).setID(kFollowBassPick).setTicked(followBassChoice_.empty()));
    kickPick.addSeparator();
    bassPick.addSeparator();
    for (size_t i = 0; i < d.tracks.size() && i < 150; ++i) {
      const auto& tr = d.tracks[i];
      if (!doc::playsPatterns(tr)) continue;
      kickPick.addItem(juce::PopupMenu::Item(u8(tr.name)).setID(kFollowKickPick + 1 + (int)i).setTicked(!followKickChoice_.empty() && tr.id == kick));
      bassPick.addItem(juce::PopupMenu::Item(u8(tr.name)).setID(kFollowBassPick + 1 + (int)i).setTicked(!followBassChoice_.empty() && tr.id == bass));
    }
    follow.addSubMenu(u8("Kick track"), kickPick);
    follow.addSubMenu(u8("Bass track"), bassPick);
    follow.addSeparator();
    follow.addSubMenu(u8("Late or early"), offset);
    follow.addSubMenu(u8("Loose"), loose);
    follow.addSeparator();
    const bool isKick = here && here->id == kick;
    const bool on = here && doc::followLink(*here);
    follow.addItem(juce::PopupMenu::Item(isKick ? u8("On: this kick always follows the STUTTER") : u8("On: this track always follows the kick in the STUTTER"))
                       .setID(kFollowOn).setTicked(on).setEnabled(here != nullptr && doc::playsPatterns(*here) && (isKick || !kick.empty())));
    m.addSubMenu("FOLLOW", follow);
  }

  // Randomize: a new draw at every click (Ctrl+Z, then again, for another).
  juce::PopupMenu random;
  random.addItem(kShuffle, drum ? u8("Shuffle the loudness (rhythm stays)") : u8("Shuffle the pitches (rhythm stays)"));
  if (!drum) random.addItem(kRandomPitch, u8("Random pitches (about one note in three, in the notes' range)"));
  random.addItem(kHumanizeMore, u8("Humanize more (bigger timing and velocity moves)"));
  random.addSeparator();
  random.addItem(kPCore, u8("New pattern from a style… (Patterns window)"));
  m.addSubMenu("Randomize", random);

  juce::PopupMenu build;
  build.addSectionHeader("Ends where the shown bars end");
  for (int b : {1, 2, 4, 8}) build.addItem(juce::PopupMenu::Item(juce::String(b) + (b == 1 ? " bar" : " bars")).setID(kBuild + b)
                                               .setEnabled((int64_t)b * bar <= to - from));
  const double tempo = bpm ? std::clamp(bpm(), 40.0, 600.0) : 145.0;
  for (int sec : {5, 10, 15, 30}) {
    const int b = std::max(1, (int)std::lround(sec * tempo / 240.0));
    build.addItem(juce::PopupMenu::Item(juce::String(sec) + u8(" s (≈ ") + juce::String(b) + (b == 1 ? " bar)" : " bars)"))
                      .setID(kBuildSec + sec).setEnabled((int64_t)b * bar <= to - from));
  }
  build.addSeparator();
  build.addItem(juce::PopupMenu::Item(u8("Roll (1/32) in the last quarter")).setID(kBuildRoll).setTicked(buildRoll_));
  build.addItem(juce::PopupMenu::Item(u8("One beat of silence before the drop")).setID(kBuildGap).setTicked(buildGap_));
  build.addSeparator();
  juce::PopupMenu onTrack;
  for (int b : {2, 4, 8}) onTrack.addItem(kBuildTrack + b, juce::String(b) + u8(" bars, ending at the next section (the drop)"));
  build.addSubMenu(u8("On a new track (snare from the library)"), onTrack, onBuildupTrack != nullptr);
  build.addSeparator();
  build.addItem(kBuildupWindow, u8("Build-up → roll → drop…"));
  m.addSubMenu("Build-up", build);

  // The tools that play through the role map (Settings > Roles): the role's plugin goes on this track, its macros move.
  juce::PopupMenu fx;
  fx.addSectionHeader(u8("Plugins of the roles (Settings > Roles)"));
  juce::PopupMenu suck;
  for (int b : {1, 2, 4}) suck.addItem(juce::PopupMenu::Item(juce::String(b) + (b == 1 ? " bar" : " bars")).setID(kSuck + b).setEnabled((int64_t)b * bar <= to - from));
  fx.addSubMenu(u8("SUCK into the drop (ends where the shown bars end)"), suck);
  juce::PopupMenu pumShown, pumSong;
  {
    const auto& tracks = host_.currentDocument().tracks;
    for (size_t i = 0; i < tracks.size() && i < 200; ++i) {
      if (tracks[i].id == trackId_ || tracks[i].instrument.kind == "audio" || tracks[i].instrument.kind == "bus") continue;
      pumShown.addItem(kPumShown + (int)i + 1, u8(tracks[i].name));
      pumSong.addItem(kPumSong + (int)i + 1, u8(tracks[i].name));
    }
  }
  fx.addSubMenu(u8("PUM over the bars shown, from"), pumShown);
  fx.addSubMenu(u8("PUM over the whole song, from"), pumSong);
  juce::PopupMenu buildFx;
  buildFx.addItem(kBuildFx + 1, u8("Filter opens + reverb grows"));
  buildFx.addItem(kBuildFx + 2, u8("Filter + reverb + delay"));
  buildFx.addItem(kBuildFx + 3, u8("Filter + reverb + delay + drive"));
  fx.addSubMenu(u8("Build-up effects up to the drop (the bars shown)"), buildFx);
  m.addSubMenu(u8("Plugin tools"), fx);

  m.addSeparator();
  m.addItem(kPCore, u8("Patterns window…"));
  m.addItem(juce::PopupMenu::Item(u8("Save the bars shown as a pattern…")).setID(kSave).setEnabled(onSavePattern != nullptr && to > from));
  m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&edit_), [safe = juce::Component::SafePointer<PatternEditor>(this), from, to, tempo](int r) {
    if (!safe || r <= 0) return;
    auto& self = *safe;
    if (r == kPCore) { if (self.onOpenPatterns) self.onOpenPatterns(); return; }
    if (r == kBuildupWindow) { self.showBuildup(); return; }
    if (r > kSuck && r <= kSuck + 4) { self.roleTool("suck", r - kSuck, -1, from, to); return; }
    if (r > kPumShown && r <= kPumShown + 200) { self.roleTool("pump", 0, r - kPumShown - 1, from, to); return; }
    if (r > kPumSong && r <= kPumSong + 200) { self.roleTool("pumpSong", 0, r - kPumSong - 1, from, to); return; }
    if (r > kBuildFx && r <= kBuildFx + 3) { self.roleTool("buildupFx", r - kBuildFx, -1, from, to); return; }
    if (r == kQuantize) { self.quantize(); return; }
    if (r >= kStrength && r <= kStrength + 100) {
      self.quantizeStrength = (r - kStrength) / 100.0;
      self.say(u8("Strength ") + juce::String(r - kStrength) + u8("%: Timing > Quantize uses it now."));
      return;
    }
    if (r == kPreview) {
      std::vector<doc::Note> notes;
      if (const doc::Track* t = self.host_.currentDocument().findTrack(self.trackId_))
        if (const doc::Pattern* p = t->findPattern(self.patternId_))
          for (const auto& n : p->notes) if (self.selected_.count(n.id)) notes.push_back(n);
      std::sort(notes.begin(), notes.end(), [](const doc::Note& a, const doc::Note& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
      if (!notes.empty() && self.onPreviewNotes) self.onPreviewNotes(self.trackId_, notes);
      return;
    }
    if (r == kHumanize) { self.humanize(); return; }
    if (r >= kSwing && r <= kSwing + 100) {
      self.swing = (r - kSwing) / 100.0;
      self.say(u8("Swing ") + juce::String(r - kSwing) + u8("%: Timing > Quantize uses it now."));
      return;
    }
    if (r >= kTranspose && r <= kTranspose + 48) { self.noteTool("transpose", r - kTranspose - 24); return; }
    if (r > kChord && r <= kChord + 4) { self.noteTool("chord", r - kChord); return; }
    if (r > kLength && r <= kLength + 3) { self.noteTool("length", r - kLength); return; }
    if (r == kLouder || r == kSofter) { self.noteTool("velocity", r == kLouder ? 1 : -1); return; }
    if (r == kShuffle || r == kRandomPitch || r == kHumanizeMore) { self.noteTool("random", r - kShuffle); return; }
    if (r > kStutter && r <= kStutter + (int)doc::stutterPresets().size()) { self.stutter(doc::stutterPresets()[(size_t)(r - kStutter - 1)].id); return; }
    if (r == kFollowZone || r == kFollowKick || r == kFollowBass || r == kFollowOn) { self.follow(r - kFollowZone); return; }
    if ((r >= kFollowKickPick && r < kFollowKickPick + 200) || (r >= kFollowBassPick && r < kFollowBassPick + 200)) {
      const bool isKick = r < kFollowBassPick;
      const int i = r - (isKick ? kFollowKickPick : kFollowBassPick) - 1;
      const auto& tracks = self.host_.currentDocument().tracks;
      std::string& choice = isKick ? self.followKickChoice_ : self.followBassChoice_;
      choice = i >= 0 && i < (int)tracks.size() ? tracks[(size_t)i].id : std::string();
      const doc::Track* c = self.host_.currentDocument().findTrack(isKick ? self.followKickTrack() : self.followBassTrack());
      self.say((isKick ? u8("FOLLOW: the kick is ") : u8("FOLLOW: the bass is ")) + (c ? u8(c->name) : u8("(none)")) + (choice.empty() ? u8(" (automatic).") : u8(".")));
      return;
    }
    if (r >= kFollowOffset && r < kFollowOffset + 7) {
      static const int64_t offsets[] = {-120, -60, -30, 0, 30, 60, 120};
      self.followOffset_ = offsets[r - kFollowOffset];
      self.say(u8("FOLLOW: ") + (self.followOffset_ == 0 ? u8("exactly on the stutter.") : juce::String(std::abs(self.followOffset_)) + u8(" ticks ") +
                                 (self.followOffset_ > 0 ? u8("late.") : u8("early."))));
      return;
    }
    if (r >= kFollowLoose && r < kFollowLoose + 4) {
      static const int64_t looses[] = {0, 15, 30, 60};
      self.followLoose_ = looses[r - kFollowLoose];
      self.say(self.followLoose_ == 0 ? u8("FOLLOW: tight, every note on time.") : u8("FOLLOW: each note moves at random by up to ") + juce::String(self.followLoose_) + u8(" ticks."));
      return;
    }
    doc::NormalizeOptions n;
    n.velocity = "keep";
    n.length = false;
    if (r == kNormEqual) { n.velocity = "equal"; self.normalize(n, u8("Normalize velocity")); }
    else if (r == kNormAccent) { n.velocity = "accent"; n.velocityValue = 0.9; self.normalize(n, u8("Accents")); }
    else if (r == kNormLength) { n.length = true; self.normalize(n, u8("Normalize length")); }
    else if (r == kNormAlign) { n.align = true; self.normalize(n, u8("Align")); }
    else if (r == kNormAll) { n.velocity = "equal"; n.length = true; n.align = true; self.normalize(n, u8("Normalize")); }
    else if (r > kMore && r <= kMore + 40) self.density("percent", r - kMore);
    else if (r > kFewer && r <= kFewer + 40) self.density("percent", -(r - kFewer));
    else if (r > kPerBeat && r <= kPerBeat + 8) self.density("perBeat", r - kPerBeat);
    else if (r >= kHow && r < kHow + 4) {
      static const char* h[] = {"fill", "ghost", "ratchet", "octave"};
      static const char* n[] = {"fills of the empty steps", "ghost notes", "ratchets", "octave copies"};
      self.densityHow = h[r - kHow];
      self.say(u8("More notes will add ") + u8(n[r - kHow]) + u8(" (choose an amount in More notes)."));
    }
    else if (r > kBuild && r <= kBuild + 8) self.buildup(r - kBuild, self.buildRoll_, self.buildGap_);
    else if (r > kBuildSec && r <= kBuildSec + 30) self.buildup(std::max(1, (int)std::lround((r - kBuildSec) * tempo / 240.0)), self.buildRoll_, self.buildGap_);
    else if (r == kBuildRoll) {
      self.buildRoll_ = !self.buildRoll_;
      self.say(self.buildRoll_ ? u8("Build-up: with a 1/32 roll in the last quarter.") : u8("Build-up: no roll at the end."));
    } else if (r == kBuildGap) {
      self.buildGap_ = !self.buildGap_;
      self.say(self.buildGap_ ? u8("Build-up: one beat of silence before the drop.") : u8("Build-up: no silence before the drop."));
    }
    else if (r > kBuildTrack && r <= kBuildTrack + 8) { if (self.onBuildupTrack) self.onBuildupTrack(r - kBuildTrack, self.buildRoll_, self.buildGap_); }
    else if (r > kVary && r <= kVary + 4) { static const char* k[] = {"octave", "reverse", "thin", "accent"}; self.vary(k[r - kVary - 1]); }
    else if (r == kSave) { if (self.onSavePattern) self.onSavePattern(from, to); }
    else if (r == kCustom) {
      auto* w = new juce::AlertWindow(lang::tr(u8("More or fewer notes")), lang::tr(u8("A percentage (+25, -15) or a number of notes (+6n, -3n), on the bars shown.")),
                                      juce::MessageBoxIconType::NoIcon, &self);
      w->addTextEditor("amount", "+25", u8("Amount:"));
      w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
      w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
      juce::Component::SafePointer<juce::AlertWindow> win(w);
      w->enterModalState(true, juce::ModalCallbackFunction::create([win, safe](int ok) {
        if (ok != 1 || !win || !safe) return;
        const juce::String text = win->getTextEditorContents("amount").trim().removeCharacters(" %");
        const bool notes = text.endsWithIgnoreCase("n");
        const double v = (notes ? text.dropLastCharacters(1) : text).getDoubleValue();
        if (v == 0 || std::abs(v) > 1000) return;
        safe->density(notes ? "count" : "percent", v);
      }), true);
    }
  });
}

// Edit > Notes and Randomize. kind: transpose (value: semitones), chord (1 octave, 2 fifth, 3 minor, 4 major),
// length (1 legato, 2 half, 3 double), velocity (+1 louder, -1 softer), random (0 shuffle, 1 random pitches,
// 2 humanize more). Random ones draw from the next seeds until one changes something (editNotesRandom).
void PatternEditor::noteTool(const std::string& kind, int value) {
  using Ids = std::set<std::string>;
  if (kind == "transpose") {
    const juce::String name = (value > 0 ? juce::String("+") : u8("−")) + juce::String(std::abs(value));
    editNotes(u8("Transpose ") + name, [=](doc::Pattern& p, const Ids& ids) { return doc::transposeNotes(p, ids, value); });
  } else if (kind == "chord") {
    static const std::vector<int> chords[] = {{12}, {7}, {3, 7}, {4, 7}};
    static const char* names[] = {"Add octave", "Add fifth", "Add minor chord", "Add major chord"};
    const auto& steps = chords[std::clamp(value, 1, 4) - 1];
    editNotes(u8(names[std::clamp(value, 1, 4) - 1]), [=](doc::Pattern& p, const Ids& ids) { return doc::addChordNotes(p, ids, steps); });
  } else if (kind == "length") {
    static const char* hows[] = {"legato", "half", "double"};
    static const char* names[] = {"Legato", "Half length", "Double length"};
    const std::string how = hows[std::clamp(value, 1, 3) - 1];
    editNotes(u8(names[std::clamp(value, 1, 3) - 1]), [=](doc::Pattern& p, const Ids& ids) { return doc::setNoteLengths(p, ids, how); });
  } else if (kind == "velocity") {
    const double factor = value > 0 ? 1.15 : 0.85;
    editNotes(value > 0 ? u8("Louder") : u8("Softer"), [=](doc::Pattern& p, const Ids& ids) { return doc::scaleVelocities(p, ids, factor); });
  } else if (kind == "random" && value == 0) {
    const bool drum = drums();
    // Shuffling swaps pitches (drums: loudness) between notes: it needs two notes that differ.
    std::set<int> kinds;
    const std::set<std::string> ids = editIds();
    const doc::Pattern* shown = pattern();
    if (!shown) return;  // the track or the pattern went (sweep: null dereference)
    for (const auto& n : shown->notes) if (ids.count(n.id)) kinds.insert(drum ? (int)std::lround(n.velocity * 100) : n.pitch);
    if (!ids.empty() && kinds.size() < 2) {
      say(juce::String(drum ? u8("Shuffle the loudness") : u8("Shuffle the pitches")) + u8(": needs 2 or more notes with different ") +
          (drum ? u8("loudness") : u8("pitches")) + u8(" in the bars shown (") + juce::String((int)ids.size()) +
          (ids.size() == 1 ? u8(" note here).") : u8(" notes here, all the same).")) +
          (drum ? juce::String() : juce::String(u8(" Try Random pitches."))));
      return;
    }
    editNotesRandom(u8("Shuffle"), [=](doc::Pattern& p, const Ids& ids, uint64_t s) { return doc::pcoreShuffle(p, ids, drum, s); });
  } else if (kind == "random" && value == 1) {
    // P-Core's chaos (pcoreApply, alter): about one note in three gets a pitch drawn in the chosen notes' range.
    const auto shown = visibleBars();
    editNotesRandom(u8("Random pitches"), [=](doc::Pattern& p, const Ids& ids, uint64_t seed) {
      int low = 127, high = 0;
      std::vector<std::pair<std::string, int>> before;
      for (const auto& n : p.notes) if (ids.count(n.id)) { low = std::min(low, n.pitch); high = std::max(high, n.pitch); before.push_back({n.id, n.pitch}); }
      if (high <= low) { low = std::max(0, low - 5); high = std::min(127, high + 7); }  // one pitch: around it
      doc::PCoreSettings s;
      s.mode = "alter";
      s.pitchChance = before.size() <= 2 ? 1.0 : 0.33;  // one or two notes: one in three would mostly change nothing
      s.pitchLow = low;
      s.pitchHigh = high;
      s.seed = seed;
      try { doc::pcoreApply(p, doc::NoteScope{shown.first, shown.second, ids}, s, {}, -1); }
      catch (const doc::DocumentError&) { return 0; }
      int changed = 0;
      for (const auto& [id, pitch] : before)
        for (const auto& n : p.notes) if (n.id == id && n.pitch != pitch) ++changed;
      return changed;
    });
  } else if (kind == "random" && value == 2) {
    const int64_t timing = std::max<int64_t>(1, snapTicks() / 4);
    editNotesRandom(u8("Humanize more (timing and loudness)"),
                    [=](doc::Pattern& p, const Ids& ids, uint64_t s) { return doc::humanizeNotes(p, ids, timing, 0.15, s); });
  }
}

// ---------------------------------------------------------------- Build-up → roll → drop (one-time use)

namespace {
bool sameNotes(const doc::Pattern& a, const doc::Pattern& b) {
  if (a.lengthTicks != b.lengthTicks || a.notes.size() != b.notes.size()) return false;
  for (size_t i = 0; i < a.notes.size(); ++i) {
    const auto &x = a.notes[i], &y = b.notes[i];
    if (x.id != y.id || x.tick != y.tick || x.length != y.length || x.pitch != y.pitch || x.velocity != y.velocity) return false;
  }
  return true;
}
bool sameTempo(const std::vector<doc::TempoPoint>& a, const std::vector<doc::TempoPoint>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) if (a[i].tick != b[i].tick || a[i].bpm != b[i].bpm || a[i].ramp != b[i].ramp) return false;
  return true;
}
}  // namespace



namespace {
// Where pattern ticks [from, to) play in the song: through the pattern's first clip. {-1, -1} when it is not in the song.
std::pair<int64_t, int64_t> songSpanOf(const doc::Track& t, const std::string& patternId, int64_t from, int64_t to) {
  for (const auto& c : t.clips)
    if (c.patternId == patternId) return {c.startTick + from - c.offsetTicks, c.startTick + to - c.offsetTicks};
  return {-1, -1};
}
juce::String sentenceOf(const doc::RoleToolResult& r) {
  juce::String said = u8(r.sentence) + ".";
  for (const auto& s : r.skipped) said << u8(" Left out: ") << u8(s) << ".";
  return said;
}
}  // namespace

void PatternEditor::roleTool(const std::string& tool, int amount, int sourceIndex, int64_t from, int64_t to) {
  const doc::Track* t = currentTrack();
  if (!t) return;
  std::string problem;
  const doc::RoleMap roles = PluginCatalog::roles(&problem);
  if (!problem.empty()) { say(u8("The role map could not be read (") + u8(problem) + u8("): Settings > Roles.")); return; }
  const doc::Document& now = host_.currentDocument();
  const std::string trackId = trackId_;
  auto span = songSpanOf(*t, patternId_, from, to);
  if (tool == "pumpSong") span = {0, now.lengthTicks};
  if (span.first < 0) { say(u8("This pattern is not in the song yet: put a clip of it in the song first.")); return; }
  span.second = std::min(span.second, now.lengthTicks);
  span.first = std::max<int64_t>(0, span.first);
  std::string source;
  if (sourceIndex >= 0 && sourceIndex < (int)now.tracks.size()) source = now.tracks[(size_t)sourceIndex].id;
  doc::RoleToolResult result;
  bool ok = false;
  const juce::String label = tool == "suck" ? u8("SUCK") : tool == "buildupFx" ? u8("Build-up effects") : u8("PUM");
  host_.applyEdit([&](doc::Document& d) {
    if (tool == "suck") result = doc::toolSuck(d, trackId, roles, span.second, (int64_t)amount * 4 * doc::kPpq);
    else if (tool == "buildupFx") {
      doc::BuildupFx fx;
      fx.delay = amount >= 2;
      fx.drive = amount >= 3;
      result = doc::toolBuildupFx(d, trackId, roles, span.first, span.second, fx);
    } else result = doc::toolPump(d, trackId, source, roles, span.first, span.second);
    ok = true;
  }, label);
  // Not written (the edit said why: a role with no plugin, no notes...).
  if (!ok || result.keys.empty() || !doc::findAutomation(host_.currentDocument(), trackId, result.keys.front())) return;
  documentChanged();
  say(sentenceOf(result));
}

void PatternEditor::showBuildup() {
  const doc::Track* t = currentTrack();
  if (!t) { say(u8("Select a note track first: the build-up is written into its pattern.")); return; }
  const auto shown = visibleBars();
  const int64_t bar = 4 * doc::kPpq;
  BuildupPanel::Start start;
  start.fromBar = (int)(shown.first / bar) + 1;
  start.toBar = std::max(start.fromBar, (int)(shown.second / bar));
  if (!selected_.empty()) {  // the bars of the selected notes
    int64_t lo = std::numeric_limits<int64_t>::max(), hi = 0;
    for (const auto& n : pattern()->notes) if (selected_.count(n.id)) { lo = std::min(lo, n.tick); hi = std::max(hi, n.tick); }
    if (lo <= hi) { start.fromBar = (int)(lo / bar) + 1; start.toBar = (int)(hi / bar) + 1; }
  }
  start.key = drums() ? -1 : doc::trackRoot(*t, pattern());
  start.bpm = bpm ? bpm() : 145.0;
  start.tempoPossible = false;
  for (const auto& c : t->clips) start.tempoPossible |= c.patternId == patternId_;
  if (patternLibrary) start.library = patternLibrary();
  if (start.library.empty()) { say(u8("Build-up: the pattern library is empty (patterns/ beside Core.exe).")); return; }
  auto panel = std::make_unique<BuildupPanel>(start);
  panel->onWrite = [safe = juce::Component::SafePointer<PatternEditor>(this)](const doc::PCoreSettings& s, int64_t from, int64_t to, double tempoTo,
                                                                               int effects, bool again) {
    return safe ? safe->buildupTake(s, from, to, tempoTo, again, effects) : juce::String(u8("The piano roll closed."));
  };
  lastTake_.reset();
  dockTool(std::move(panel), u8("BUILD-UP"), [this] { showBuildup(); });
}

juce::String PatternEditor::buildupTake(const doc::PCoreSettings& settings, int64_t from, int64_t to, double tempoTo, bool again, int effects) {
  const doc::Track* t = currentTrack();
  if (!t) return u8("Select a note track first.");
  const std::vector<doc::PatternPreset> lib = patternLibrary ? patternLibrary() : std::vector<doc::PatternPreset>{};
  doc::PCoreSettings s = settings;
  s.seed = grooveSeed_++;
  const std::string trackId = trackId_, patternId = patternId_;
  const int key = drums() ? -1 : doc::trackRoot(*t, pattern());
  std::set<std::string> ids;
  for (const auto& id : selected_) ids.insert(id);
  const doc::NoteScope scope{from, to, ids};
  // "Again" takes the last take back only when nothing else changed the pattern or the tempo since.
  const doc::Document& now = host_.currentDocument();
  const bool undoLast = again && lastTake_ && lastTake_->trackId == trackId && lastTake_->patternId == patternId &&
                        sameNotes(*pattern(), lastTake_->after) && sameTempo(now.tempoMap, lastTake_->tempoAfter);
  const int64_t bar = 4 * doc::kPpq;
  // The song ticks of the transition (for the tempo): the first clip of this pattern.
  int64_t songFrom = -1, songTo = -1;
  if (tempoTo > 0 || effects > 0)
    for (const auto& c : t->clips)
      if (c.patternId == patternId) {
        const int64_t transition = s.buildupStart >= 0 ? s.buildupStart : s.rollStart >= 0 ? s.rollStart : from;
        songFrom = c.startTick + transition - c.offsetTicks;
        songTo = c.startTick + to - c.offsetTicks;
        break;
      }
  int written = 0;
  doc::RoleToolResult fxResult;
  std::string fxProblem;
  const doc::RoleMap roles = effects > 0 ? PluginCatalog::roles(&fxProblem) : doc::RoleMap{};
  PCoreTake take;
  take.trackId = trackId;
  take.patternId = patternId;
  bool ok = false;
  host_.applyEdit([&](doc::Document& d) {
    doc::Pattern& p = doc::patternById(doc::trackById(d, trackId), patternId);
    if (undoLast) { p = lastTake_->before; d.tempoMap = lastTake_->tempoBefore; }
    take.before = p;
    take.tempoBefore = d.tempoMap;
    const int64_t need = to + (s.dropTemplate.empty() ? 0 : (int64_t)s.dropBars * bar);
    if (need > p.lengthTicks) doc::extendPatternRepeating(p, std::min<int64_t>(need, 64LL * bar));
    written = doc::pcoreApply(p, scope, s, lib, key);
    if (tempoTo > 0 && songFrom >= 0 && songTo > songFrom) doc::tempoRampTo(d, songFrom, songTo, tempoTo);
    // The roles' effects up to the drop, in the same undo step. A role with no plugin leaves the notes written and says so.
    if (effects > 0 && songFrom >= 0 && songTo > songFrom) {
      try {
        doc::BuildupFx fx;
        fx.delay = effects >= 2;
        fx.drive = effects >= 3;
        fxResult = doc::toolBuildupFx(d, trackId, roles, std::max<int64_t>(0, songFrom), std::min(songTo, d.lengthTicks), fx);
      } catch (const std::exception& e) {
        fxProblem = e.what();
      }
    }
    take.after = p;
    take.tempoAfter = d.tempoMap;
    ok = true;
  }, undoLast ? u8("Build-up (another take)") : u8("Build-up"));
  if (!ok) return u8("Nothing written (see the message).");
  lastTake_ = take;
  documentChanged();
  juce::String said = juce::String(written) + u8(" notes in bars ") + juce::String(from / bar + 1) + u8("–") + juce::String(to / bar);
  if (s.roll != "off") said << u8(", drop at bar ") << juce::String(to / bar + 1);
  if (effects > 0) {
    if (songFrom < 0) said << u8(" (no effects: the pattern is not in the song)");
    else if (!fxProblem.empty()) said << u8(" (no effects: ") << u8(fxProblem) << ")";
    else said << ". " << u8(fxResult.sentence);
  }
  if (tempoTo > 0) said << (songFrom >= 0 ? u8(", tempo up to ") + juce::String(tempoTo, 0) + " BPM" : u8(" (tempo unchanged: the pattern is not in the song)"));
  said << (undoLast ? u8(". Another take.") : u8("."));
  say(u8("Build-up: ") + said);
  return said;
}

// ---------------------------------------------------------------- Pattern Core (P-Core) (layers, selected tracks)

// STUTTER (note slice, CORE-MANUAL stutter-voice-stutter-execution-plan.md INT-02): the selected notes (or the bars
// shown) become ordinary repeats written by the named recipe; one undo step gives the original back.
void PatternEditor::stutter(const std::string& presetId) {
  const doc::StutterPreset* p = doc::findStutterPreset(presetId);
  if (!p) return;
  const bool pitched = !drums();
  const doc::StutterRecipe recipe = p->recipe;
  const std::string id = presetId;
  editNotes(u8("STUTTER: ") + u8(p->name), [=](doc::Pattern& pat, const std::set<std::string>& ids) {
    return doc::stutterNotes(pat, ids, recipe, id, pitched);
  });
}

void PatternEditor::vary(const std::string& kind) {
  const uint64_t seed = grooveSeed_++;
  editNotes(u8("Variation: ") + u8(kind), [=](doc::Pattern& p, const std::set<std::string>& ids) { return doc::varyNotes(p, ids, kind, seed); });
}

// Every note drawn at x (selected ones only, when the one under x is selected) gets the velocity of the height y.
void PatternEditor::setVelocityAt(int x, int y) {
  const auto lane = velocityLane();
  const double v = std::clamp(std::round((lane.getBottom() - y) / (double)std::max(1, lane.getHeight() - 4) * 127.0), 1.0, 127.0) / 127.0;
  std::set<std::string> ids;
  bool anySelected = false;
  for (const auto& n : pattern()->notes) {
    const int x0 = xOfTick(n.tick), w = juce::jlimit(3, drums() ? 14 : 8, xOfTick(n.tick + n.length) - x0 - 2);
    if (x >= x0 - 2 && x <= x0 + w + 2) { ids.insert(n.id); anySelected |= selected_.count(n.id) > 0; }
  }
  if (anySelected) for (auto it = ids.begin(); it != ids.end();) it = selected_.count(*it) ? std::next(it) : ids.erase(it);
  if (ids.empty()) return;
  const std::string trackId = trackId_, patternId = patternId_;
  host_.applyEdit([&](doc::Document& d) {
    for (auto& n : doc::patternById(doc::trackById(d, trackId), patternId).notes)
      if (ids.count(n.id)) n.velocity = v;
  }, u8("Velocity"));
}

void PatternEditor::eraseAt(juce::Point<int> p) {
  const doc::Note* hit = noteAt(p, nullptr);
  if (!hit) return;
  const std::string trackId = trackId_, patternId = patternId_, id = hit->id;
  selected_.erase(id);
  host_.applyEdit([&](doc::Document& d) {
    auto& notes = doc::patternById(doc::trackById(d, trackId), patternId).notes;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const doc::Note& n) { return n.id == id; }), notes.end());
  }, u8("Erase notes"));
}

// Zoom by `factor` (< 1: in) keeping the tick `at` where it is; down to one beat across the grid.
void PatternEditor::zoom(double factor, double at) {
  if (!currentTrack()) return;
  const double len = (double)pattern()->lengthTicks;
  const double length = std::clamp(shownLength() * factor, std::min(len, (double)doc::kPpq), len);
  viewStart_ = at - (at - shownStart()) * length / shownLength();
  viewLength_ = length;  // the whole pattern too (drums would otherwise go back to the first view)
  placePaging();
  repaint();
}

void PatternEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
  struct After { PatternEditor& ed; juce::Point<int> p; ~After() { ed.updateHover(p); } } after{*this, e.getPosition()};  // keys or zoom moved under the mouse
  // Ctrl+wheel: zoom in or out around the mouse (down to a beat across the grid). Shift+wheel (or a sideways
  // swipe) while zoomed: move along the pattern.
  if (currentTrack() && (e.mods.isCtrlDown() || e.mods.isCommandDown())) {
    zoom(std::pow(2.0, -wheel.deltaY * 2.0), shownStart() + std::clamp((e.x - grid().getX()) / (double)std::max(1, grid().getWidth()), 0.0, 1.0) * shownLength());
    return;
  }
  // Alt+wheel: bigger or smaller keys (fewer or more of them), the key under the mouse kept in place.
  if (currentTrack() && !drums() && e.mods.isAltDown()) {
    const int under = pitchAtY(e.y), before = keysShown();
    rowsWanted_ = std::clamp(before + (wheel.deltaY > 0 ? -2 : 2), 8, 61);
    if (under >= 0) lowPitch_ = std::clamp(under - (under - lowPitch_) * keysShown() / std::max(1, before), 0, 128 - keysShown());
    repaint();
    return;
  }
  if (currentTrack() && shownLength() < (double)pattern()->lengthTicks && (e.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY))) {
    const double delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    viewStart_ = shownStart() - delta * shownLength() * 0.5;
    placePaging();
    repaint();
    return;
  }
  const doc::Note* hit = noteAt(e.getPosition(), nullptr);
  if (!hit && currentTrack() && velocityLane().contains(e.getPosition()))
    for (const auto& n : pattern()->notes)
      if (const int x0 = xOfTick(n.tick); e.x >= x0 - 2 && e.x <= x0 + 10) { hit = &n; break; }
  if (!hit) {
    if (!drums()) { lowPitch_ = std::clamp(lowPitch_ + (wheel.deltaY > 0 ? 1 : -1), 0, 128 - keysShown()); repaint(); }
    return;
  }
  const std::string trackId = trackId_, patternId = patternId_, id = hit->id;
  const double step = wheel.deltaY > 0 ? 0.05 : -0.05;
  host_.applyEdit([&](doc::Document& d) {
    for (auto& n : doc::patternById(doc::trackById(d, trackId), patternId).notes)
      if (n.id == id) n.velocity = std::clamp(std::round((n.velocity + step) * 100) / 100, 0.05, 1.0);
  }, "Velocity");
}

bool PatternEditor::keyPressed(const juce::KeyPress& key) {
  const Keymap& keys = Keymap::get();  // File > Settings > Keyboard shortcuts
  if (keys.matches("edit.delete", key)) { deleteSelection(); return true; }
  if (keys.matches("edit.duplicate", key)) { duplicateSelection(); return true; }
  if (keys.matches("edit.selectAll", key)) { selectAll(); return true; }
  return false;  // the window handles the rest (Space, undo...)
}

}  // namespace core::app
