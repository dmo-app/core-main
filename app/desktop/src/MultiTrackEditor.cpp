#include "MultiTrackEditor.h"
#include <cmath>

namespace core::app {

namespace {
juce::String u8(const char* t) { return juce::String::fromUTF8(t); }
juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }
std::string key(const std::string& patternId, const std::string& noteId) { return patternId + "|" + noteId; }
}  // namespace

std::vector<std::pair<juce::Rectangle<int>, std::string>> MultiTrackEditor::chips() const {
  std::vector<std::pair<juce::Rectangle<int>, std::string>> out;
  const doc::Document& d = host_.currentDocument();
  juce::Font f(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  int x = 66;
  for (const auto& t : d.tracks) {
    if (!doc::playsPatterns(t)) continue;
    const int w = (int)std::ceil(juce::GlyphArrangement::getStringWidth(f, u8(t.name))) + 24;
    if (x + w > chipsRight_ - 4) break;  // the rest: "+ Arrange selection" adds them
    out.push_back({{x, 5, w, kChips - 10}, t.id});
    x += w + 4;
  }
  return out;
}

// One note grid, as Ableton's multi-clip editing and Bitwig's layered editing do it (Diogo 2026-11-16: "vê como as
// outras DAWs funcionam"): the keys on the left, the notes of every track shown here in the same grid, each in its
// track's colour; the active track's notes solid, the others dimmed (a click on one makes its track active). Fold
// (on by default, as in both) shows only the keys that play, with the drum sounds named.
std::vector<MultiTrackEditor::Lane> MultiTrackEditor::lanes() const {
  const doc::Document& d = host_.currentDocument();
  const int64_t a = from(), b = to();
  const auto area = lanesArea();
  Lane l;
  l.trackId = active_;
  std::set<int> used;
  for (const auto& id : shown_) {
    for (int p : doc::songPitchesIn(d, id, a, b)) used.insert(p);
    if (const doc::Track* t = d.findTrack(id); t && used.empty()) used.insert(defaultPitch(*t));
  }
  if (used.empty()) used.insert(60);
  if (fold_) {
    l.rows.assign(used.rbegin(), used.rend());
  } else {
    const int lo = std::max(0, *used.begin() - 12), hi = std::min(127, *used.rbegin() + 12);
    for (int p = hi; p >= lo; --p) l.rows.push_back(p);
  }
  // Rows as tall as fit (folded: up to 40 px), never thinner than 14 px (then the keys scroll); Alt+wheel sets it.
  const float fit = (float)(area.getHeight() - 6) / (float)l.rows.size();
  l.rowH = rowH_ > 0 ? rowH_ : std::clamp(fit, 14.0f, fold_ ? 40.0f : 18.0f);
  const int h = (int)std::ceil(l.rows.size() * l.rowH) + 6;
  l.box = {area.getX(), area.getY() - scrollY_, area.getWidth(), std::max(h, area.getHeight())};
  const_cast<MultiTrackEditor*>(this)->lanesHeight_ = h;
  return {l};
}

juce::String MultiTrackEditor::keyName(int pitch) const {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const juce::String note = juce::String(names[pitch % 12]) + juce::String(pitch / 12 - 2);
  // A drum track playing this key: the sound's name first (Ableton's drum rack and Bitwig's drum machine name rows).
  const doc::Document& d = host_.currentDocument();
  for (const auto& id : shown_) {
    const doc::Track* t = d.findTrack(id);
    if (!t || !drumTrack(*t)) continue;
    const char* drum = pitch == 35 || pitch == 36 ? "Kick" : pitch == 38 || pitch == 40 ? "Snare" : pitch == 39 ? "Clap" : pitch == 42 ? "Closed hat"
                     : pitch == 44 ? "Pedal hat" : pitch == 46 ? "Open hat" : pitch == 49 || pitch == 57 ? "Crash" : pitch == 51 || pitch == 59 ? "Ride"
                     : pitch == 70 ? "Shaker" : nullptr;
    if (drum) return juce::String(drum) + "  " + note;
  }
  return note;
}

std::optional<MultiTrackEditor::Hit> MultiTrackEditor::hitAt(const Lane& l, juce::Point<int> p) const {
  // The active track's notes first, then the others (on top of each other as drawn).
  std::vector<std::string> order{active_};
  for (const auto& id : shown_) if (id != active_) order.push_back(id);
  for (const auto& id : order) {
    const auto notes = doc::songNotesIn(host_.currentDocument(), id, from(), to());
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
      const auto r = noteRect(l, it->start, std::max<int64_t>(it->length, snapTicks() / 2), it->pitch).expanded(0.0f, 1.0f);
      if (r.contains(p.toFloat()))
        return Hit{it->patternId, it->noteId, it->start, it->length, it->pitch, p.x >= r.getRight() - 5 && r.getWidth() > 12, id};
    }
  }
  return std::nullopt;
}

void MultiTrackEditor::paint(juce::Graphics& g) {
  const doc::Document& d = host_.currentDocument();
  g.fillAll(theme::kPanel);
  // The tracks in the editor: a chip each, lit when shown, outlined when it is the active one.
  g.setColour(theme::kDim);
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  g.drawText("TRACKS", juce::Rectangle<int>(8, 0, 56, kChips), juce::Justification::centredLeft, false);
  for (const auto& [r, id] : chips()) {
    const doc::Track* t = d.findTrack(id);
    const bool on = std::find(shown_.begin(), shown_.end(), id) != shown_.end();
    g.setColour(on ? theme::kControlHover : theme::kControl);
    g.fillRoundedRectangle(r.toFloat(), 3.0f);
    if (id == active_) { g.setColour(theme::kText); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 3.0f, 1.0f); }
    g.setColour(on ? trackColour(*t) : theme::kDim);
    g.fillRoundedRectangle(juce::Rectangle<float>((float)r.getX() + 6, (float)r.getCentreY() - 3, 6, 6), 1.5f);
    g.setColour(on ? theme::kText : theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
    g.drawText(u8(t->name), r.withTrimmedLeft(16).withTrimmedRight(4), juce::Justification::centredLeft, true);
  }
  // The ruler: bar numbers, beats (bar.beat) and sixteenths (bar.beat.16th) as the zoom gives room, as both DAWs do.
  // Drag it up or down to zoom, sideways to move along; a click moves the play position.
  const auto ruler = rulerArea();
  const int64_t a = from(), b = to();
  const int64_t grid = snapTicks();
  const double beatPx = (double)ruler.getWidth() * doc::kPpq / (double)std::max<int64_t>(1, b - a);
  g.setColour(theme::kField);
  g.fillRect(ruler);
  for (int64_t t = a / grid * grid; t <= b; t += grid) {
    if (t < a) continue;
    const int x = xOf(t);
    const bool bar = t % kBar == 0, beat = t % doc::kPpq == 0;
    g.setColour(bar ? theme::kText : beat ? theme::kMuted : theme::kDim);
    g.drawVerticalLine(x, (float)ruler.getBottom() - (bar ? 16.0f : beat ? 10.0f : 5.0f), (float)ruler.getBottom());
    if (bar) {
      g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
      g.drawText(juce::String(t / kBar + 1), x + 3, ruler.getY() + 1, 50, 16, juce::Justification::centredLeft, false);
    } else if (beat && beatPx >= 34) {
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String(t / kBar + 1) + "." + juce::String((t % kBar) / doc::kPpq + 1), x + 3, ruler.getY() + 3, 50, 14,
                 juce::Justification::centredLeft, false);
    } else if (!beat && beatPx >= 160 && t % (doc::kPpq / 4) == 0) {
      g.setColour(theme::kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String(t / kBar + 1) + "." + juce::String((t % kBar) / doc::kPpq + 1) + "." + juce::String((t % doc::kPpq) / (doc::kPpq / 4) + 1),
                 x + 2, ruler.getY() + 3, 60, 14, juce::Justification::centredLeft, false);
    }
  }
  // The note grid.
  const auto area = lanesArea();
  const Lane l = lanes().front();
  const auto keys = juce::Rectangle<int>(area.getX(), area.getY(), kHead, area.getHeight());
  const auto notes = area.withTrimmedLeft(kHead);
  g.saveState();
  g.reduceClipRegion(area);
  g.setColour(theme::kField);
  g.fillRect(notes);
  for (size_t i = 0; i < l.rows.size(); ++i) {
    const int pitch = l.rows[i];
    const int y = (int)(l.box.getY() + 3 + i * l.rowH);
    const bool black = juce::String("0101001010100").substring(pitch % 12, pitch % 12 + 1) == "1";
    if (black && !fold_) { g.setColour(theme::kBackground.withAlpha(0.35f)); g.fillRect(notes.getX(), y, notes.getWidth(), (int)l.rowH); }
    g.setColour(black ? theme::kCard.darker(0.3f) : theme::kCard.brighter(0.05f));
    g.fillRect(keys.getX(), y, keys.getWidth(), (int)std::ceil(l.rowH));
    g.setColour(theme::kGrid);
    g.drawHorizontalLine(y, (float)keys.getX(), (float)notes.getRight());
    g.setColour(theme::kMuted);
    g.setFont(juce::FontOptions(l.rowH >= 18 ? theme::kFontBody : theme::kFontSmall));
    if (l.rowH >= 11) g.drawText(keyName(pitch), keys.getX() + 8, y, keys.getWidth() - 12, (int)l.rowH, juce::Justification::centredLeft, true);
  }
  for (int64_t t = a / grid * grid; t < b; t += grid) {
    if (t < a) continue;
    g.setColour(t % kBar == 0 ? theme::kMuted.withAlpha(0.55f) : t % doc::kPpq == 0 ? theme::kOutline : theme::kGrid);
    g.drawVerticalLine(xOf(t), (float)notes.getY(), (float)notes.getBottom());
  }
  // Where the active track has no clip: shaded (a note there makes a one-bar clip).
  if (const doc::Track* tr = d.findTrack(active_)) {
    std::vector<std::pair<int64_t, int64_t>> spans;
    for (const auto& c : tr->clips) spans.push_back({c.startTick, c.startTick + c.lengthTicks});
    std::sort(spans.begin(), spans.end());
    int64_t at = a;
    g.setColour(theme::kBackground.withAlpha(0.4f));
    for (const auto& [s0, e0] : spans) {
      if (s0 > at) g.fillRect(juce::Rectangle<int>::leftTopRightBottom(xOf(at), notes.getY(), xOf(std::min(s0, b)), notes.getBottom()));
      at = std::max(at, e0);
    }
    if (at < b) g.fillRect(juce::Rectangle<int>::leftTopRightBottom(xOf(at), notes.getY(), notes.getRight(), notes.getBottom()));
  }
  // The notes: the other tracks first (dimmed), the active one on top.
  std::map<int64_t, int> starts;
  g.reduceClipRegion(notes);
  std::vector<std::string> order;
  for (const auto& id : shown_) if (id != active_) order.push_back(id);
  if (!active_.empty()) order.push_back(active_);
  for (const auto& id : order) {
    const doc::Track* t = d.findTrack(id);
    if (!t) continue;
    const bool act = id == active_;
    const juce::Colour colour = trackColour(*t);
    for (const auto& n : doc::songNotesIn(d, id, a, b)) {
      const auto r = noteRect(l, n.start, drumTrack(*t) ? std::max<int64_t>(std::min<int64_t>(n.length, grid), grid) : n.length, n.pitch);
      if (act) {
        g.setColour(colour.withAlpha((float)(0.45 + 0.55 * n.velocity)));
        g.fillRoundedRectangle(r, 2.0f);
        g.setColour(colour.darker(0.6f));
        g.drawRoundedRectangle(r, 2.0f, 1.0f);
        if (selected_.count(key(n.patternId, n.noteId))) { g.setColour(theme::kText); g.drawRoundedRectangle(r, 2.0f, 2.0f); }
      } else {
        g.setColour(colour.withAlpha(0.22f));
        g.fillRoundedRectangle(r, 2.0f);
        g.setColour(colour.withAlpha(0.55f));
        g.drawRoundedRectangle(r, 2.0f, 1.0f);
      }
      ++starts[n.start];
    }
  }
  g.restoreState();
  if (guideTick_ >= 0 && starts.count(guideTick_) && starts[guideTick_] > 1) {
    g.setColour(theme::kStartLine.withAlpha(0.7f));
    g.drawVerticalLine(xOf(guideTick_), (float)area.getY(), (float)area.getBottom());
  }
  // The active track's velocity, a stalk per note.
  const auto vel = velArea();
  g.setColour(theme::kField);
  g.fillRect(vel);
  g.setColour(theme::kOutline);
  g.drawHorizontalLine(vel.getY(), 0.0f, (float)getWidth());
  g.setColour(theme::kDim);
  g.setFont(juce::FontOptions(theme::kFontSmall));
  g.drawText(u8("VELOCITY"), juce::Rectangle<int>(8, vel.getY(), kHead - 12, vel.getHeight()), juce::Justification::centredLeft, false);
  if (const doc::Track* t = d.findTrack(active_)) {
    g.setColour(trackColour(*t));
    for (const auto& n : doc::songNotesIn(d, active_, a, b)) {
      const int h = (int)std::round(n.velocity * (vel.getHeight() - 8));
      const int x = xOf(n.start);
      g.fillRect(x, vel.getBottom() - 3 - h, 2, h);
      g.fillEllipse((float)x - 3, (float)(vel.getBottom() - 3 - h) - 3, 8, 8);
    }
  }
  const double play = host_.playheadTick();
  if (play >= (double)a && play < (double)b) {
    g.setColour(theme::kStartLine);
    g.drawVerticalLine(xOf((int64_t)play), (float)ruler.getY(), (float)vel.getBottom());
  }
}

void MultiTrackEditor::mouseDown(const juce::MouseEvent& e) {
  grabKeyboardFocus();
  const auto p = e.getPosition();
  for (const auto& [r, id] : chips())
    if (r.contains(p)) {
      show(id, true);
      return;
    }
  if (rulerArea().contains(p)) {  // a click: the play position; a drag up/down: zoom, sideways: move along
    drag_ = Drag::Ruler;
    rulerStart_ = p;
    rulerFrom_ = from();
    rulerLen_ = to() - from();
    rulerTick_ = tickAt(p.x, false);
    return;
  }
  if (velArea().contains(p)) {
    if (active_.empty()) return;
    drag_ = Drag::Velocity;
    host_.startGesture(u8("Velocity"));
    setVelocityAt(p);
    return;
  }
  const auto ls = lanes();
  const Lane* l = laneAt(p, ls);
  if (!l || p.x < kHead) return;
  if (auto hit = hitAt(*l, p)) {
    const std::string trackId = hit->trackId;
    if (trackId != active_) {  // a dimmed note: its track becomes the active one
      active_ = trackId;
      selected_.clear();
      if (onActiveTrack) onActiveTrack(active_);
    }
    // Right-click, Alt+click or a double click: the note goes.
    if (e.mods.isPopupMenu() || e.mods.isAltDown() || e.getNumberOfClicks() > 1) {
      const auto h = *hit;
      try {
        host_.applyEdit([&](doc::Document& d) { doc::removeSongNote(d, trackId, h.patternId, h.noteId); }, u8("Delete note"));
      } catch (const std::exception&) {}
      selected_.erase(key(h.patternId, h.noteId));
      repaint();
      return;
    }
    const std::string k = key(hit->patternId, hit->noteId);
    if (e.mods.isCtrlDown()) { if (!selected_.erase(k)) selected_.insert(k); }
    else if (!selected_.count(k)) selected_ = {k};
    grabbed_ = *hit;
    dragTrack_ = trackId;
    grabOffset_ = tickAt(p.x, false) - hit->start;
    drag_ = hit->rightEdge ? Drag::Resize : Drag::Move;
    guideTick_ = hit->start;
    host_.startGesture(drag_ == Drag::Move ? u8("Move note") : u8("Length"));
    repaint();
    return;
  }
  if (active_.empty()) return;
  const std::string trackId = active_;
  if (e.mods.isPopupMenu()) return;
  // An empty place: a new note there (a drum hit one step long), grabbed so it can be dragged at once.
  const int pitch = pitchAt(*l, p.y);
  const int64_t at = tickAt(p.x, !e.mods.isShiftDown());
  std::pair<std::string, std::string> made;
  try {
    host_.startGesture(u8("Add note"));
    host_.applyEdit([&](doc::Document& d) { made = doc::addSongNote(d, trackId, at, std::max<int64_t>(snapTicks(), doc::kPpq / 16), pitch, 0.8); }, u8("Add note"));
  } catch (const std::exception&) {
    host_.finishGesture();
    return;
  }
  selected_ = {key(made.first, made.second)};
  grabbed_ = {made.first, made.second, at, snapTicks(), pitch, false, trackId};
  dragTrack_ = trackId;
  grabOffset_ = tickAt(p.x, false) - at;
  drag_ = Drag::Move;
  guideTick_ = at;
  repaint();
}

void MultiTrackEditor::mouseDrag(const juce::MouseEvent& e) {
  const auto p = e.getPosition();
  if (drag_ == Drag::Velocity) { setVelocityAt(p); return; }
  if (drag_ == Drag::Ruler) {
    const int dy = p.y - rulerStart_.y, dx = p.x - rulerStart_.x;
    if (std::abs(dy) < 3 && std::abs(dx) < 3) return;
    // Down: closer (zoom in), up: further; the tick grabbed stays under the mouse.
    const double factor = std::pow(1.012, -(double)dy);
    viewLen_ = std::clamp<int64_t>((int64_t)std::llround((double)rulerLen_ * factor), doc::kPpq, 32 * kBar);
    const double share = (double)(rulerTick_ - rulerFrom_) / (double)std::max<int64_t>(1, rulerLen_);
    const double pxPerTick = (double)std::max(1, rulerArea().getWidth()) / (double)viewLen_;
    viewFrom_ = std::max<int64_t>(0, rulerTick_ - (int64_t)(share * (double)viewLen_) - (int64_t)((double)dx / pxPerTick));
    scrolled_ = true;
    resized();
    repaint();
    return;
  }
  if (drag_ == Drag::None) return;
  const auto ls = lanes();
  const Lane* l = &ls.front();
  const std::string trackId = dragTrack_;
  const Hit h = grabbed_;
  if (drag_ == Drag::Move) {
    const int64_t wanted = e.mods.isShiftDown() ? tickAt(p.x, false) - grabOffset_
                                                : std::max<int64_t>(0, (tickAt(p.x, false) - grabOffset_ + snapTicks() / 2) / snapTicks() * snapTicks());
    const int64_t dt = wanted - h.start;
    const int dp = pitchAt(*l, std::clamp(p.y, l->box.getY(), l->box.getBottom() - 1)) - h.pitch;
    if (dt == 0 && dp == 0) return;
    try {
      host_.applyEdit([&](doc::Document& d) { doc::moveSongNote(d, trackId, h.patternId, h.noteId, dt, dp); }, u8("Move note"));
    } catch (const std::exception&) { return; }
    grabbed_.start += dt;
    grabbed_.pitch = std::clamp(grabbed_.pitch + dp, 0, 127);
    guideTick_ = grabbed_.start;
  } else {
    const int64_t end = std::max<int64_t>(h.start + 1, (tickAt(p.x, false) + snapTicks() / 2) / snapTicks() * snapTicks());
    try {
      host_.applyEdit([&](doc::Document& d) { doc::resizeSongNote(d, trackId, h.patternId, h.noteId, end - h.start); }, u8("Length"));
    } catch (const std::exception&) {}
  }
  repaint();
}

void MultiTrackEditor::mouseUp(const juce::MouseEvent& e) {
  if (drag_ == Drag::Ruler) {
    if (e.mouseWasClicked()) host_.movePlayhead(tickAt(e.x, true));
    drag_ = Drag::None;
    return;
  }
  if (drag_ != Drag::None) host_.finishGesture();
  drag_ = Drag::None;
  grabbed_ = {};
  guideTick_ = -1;
  repaint();
}

void MultiTrackEditor::setVelocityAt(juce::Point<int> p) {
  const auto vel = velArea();
  const double v = std::clamp(1.0 - (double)(p.y - vel.getY() - 2) / std::max(1, vel.getHeight() - 6), 0.01, 1.0);
  const std::string trackId = active_;
  std::vector<std::pair<std::string, std::string>> under;
  for (const auto& n : doc::songNotesIn(host_.currentDocument(), trackId, from(), to()))
    if (std::abs(xOf(n.start) + 1 - p.x) <= 3) under.push_back({n.patternId, n.noteId});
  if (under.empty()) return;
  try {
    host_.applyEdit([&](doc::Document& d) { for (const auto& [pid, nid] : under) doc::setSongNoteVelocity(d, trackId, pid, nid, v); }, u8("Velocity"));
  } catch (const std::exception&) {}
  repaint();
}

void MultiTrackEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
  const double step = (std::abs(w.deltaY) > std::abs(w.deltaX) ? w.deltaY : w.deltaX);
  if (step == 0) return;
  // Ctrl+wheel: zoom around the mouse, down to one beat.
  if (e.mods.isCtrlDown()) { zoomBy(step > 0 ? 0.75 : 1.0 / 0.75, e.x); return; }
  // Alt+wheel: taller or shorter keys (both DAWs zoom the keys this way).
  if (e.mods.isAltDown()) {
    const float now = lanes().front().rowH;
    rowH_ = std::clamp(now * (step > 0 ? 1.15f : 1.0f / 1.15f), 8.0f, 60.0f);
    repaint();
    return;
  }
  // The wheel: up and down through the lanes when they do not fit; Shift (or no room to scroll): along the song.
  const int room = std::max(0, lanesHeight_ - lanesArea().getHeight());
  if (!e.mods.isShiftDown() && room > 0 && std::abs(w.deltaY) >= std::abs(w.deltaX)) {
    scrollY_ = std::clamp(scrollY_ - (int)std::lround(step * 120), 0, room);
    repaint();
    return;
  }
  const int64_t a = from(), len = to() - from();
  viewLen_ = len;
  viewFrom_ = std::max<int64_t>(0, a + (step > 0 ? -1 : 1) * std::max<int64_t>(doc::kPpq, len / 4) / doc::kPpq * doc::kPpq);
  scrolled_ = true;
  resized();
  repaint();
}

bool MultiTrackEditor::keyPressed(const juce::KeyPress& key) {
  if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && !selected_.empty() && !active_.empty()) {
    const std::string trackId = active_;
    const auto sel = selected_;
    try {
      host_.applyEdit([&](doc::Document& d) {
        for (const auto& k : sel) {
          const auto bar = k.find('|');
          try { doc::removeSongNote(d, trackId, k.substr(0, bar), k.substr(bar + 1)); } catch (const doc::DocumentError&) {}
        }
      }, u8("Delete notes"));
    } catch (const std::exception&) {}
    selected_.clear();
    repaint();
    return true;
  }
  if (key == juce::KeyPress::escapeKey && !selected_.empty()) { selected_.clear(); repaint(); return true; }
  return false;
}

}  // namespace core::app
