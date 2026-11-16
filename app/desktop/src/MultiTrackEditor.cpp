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

std::vector<MultiTrackEditor::Lane> MultiTrackEditor::lanes() const {
  std::vector<Lane> out;
  const doc::Document& d = host_.currentDocument();
  const int64_t a = from(), b = to();
  auto area = lanesArea();
  // Each lane its natural height (drums 16 px a row, notes 8); squeezed together when they do not fit.
  std::vector<Lane> ls;
  int want = 0;
  for (const auto& id : shown_) {
    const doc::Track* t = d.findTrack(id);
    if (!t) continue;
    Lane l;
    l.trackId = id;
    l.drums = drumTrack(*t);
    std::vector<int> used = doc::songPitchesIn(d, id, a, b);
    if (used.empty()) used = {defaultPitch(*t)};
    if (l.drums) {
      l.rows.assign(used.rbegin(), used.rend());
      l.rowH = 16;
    } else {
      const int lo = std::max(0, used.front() - 1), hi = std::min(127, std::max(used.back() + 1, lo + 4));
      for (int p = hi; p >= lo; --p) l.rows.push_back(p);
      l.rowH = 8;
    }
    want += (int)(l.rows.size() * l.rowH) + 6;
    ls.push_back(l);
  }
  // Squeezed when they do not fit; when there is room, taller (up to 2.5x) so the notes are easy to grab.
  const float squeeze = want > 0 ? std::min(2.5f, (float)area.getHeight() / (float)want) : 1.0f;
  int y = area.getY();
  for (auto& l : ls) {
    l.rowH = std::max(3.0f, l.rowH * squeeze);
    const int h = std::max(22, (int)std::ceil(l.rows.size() * l.rowH) + 6);
    l.box = {area.getX(), y, area.getWidth(), h};
    y += h;
    out.push_back(l);
  }
  return out;
}

std::optional<MultiTrackEditor::Hit> MultiTrackEditor::hitAt(const Lane& l, juce::Point<int> p) const {
  const auto notes = doc::songNotesIn(host_.currentDocument(), l.trackId, from(), to());
  for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
    const auto r = noteRect(l, it->start, it->length, it->pitch).expanded(0.0f, 1.0f);
    if (r.contains(p.toFloat()))
      return Hit{it->patternId, it->noteId, it->start, it->length, it->pitch, !l.drums && p.x >= r.getRight() - 4};
  }
  return std::nullopt;
}

void MultiTrackEditor::paint(juce::Graphics& g) {
  const doc::Document& d = host_.currentDocument();
  g.fillAll(theme::kPanel);
  // Tracks: a chip each, lit when its lane shows.
  g.setColour(theme::kDim);
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  g.drawText("TRACKS", juce::Rectangle<int>(8, 0, 56, kChips), juce::Justification::centredLeft, false);
  for (const auto& [r, id] : chips()) {
    const doc::Track* t = d.findTrack(id);
    const bool on = std::find(shown_.begin(), shown_.end(), id) != shown_.end();
    g.setColour(on ? theme::kControlHover : theme::kControl);
    g.fillRoundedRectangle(r.toFloat(), 3.0f);
    g.setColour(on ? trackColour(*t) : theme::kDim);
    g.fillRoundedRectangle(juce::Rectangle<float>((float)r.getX() + 6, (float)r.getCentreY() - 3, 6, 6), 1.5f);
    g.setColour(on ? theme::kText : theme::kMuted);
    g.drawText(u8(t->name), r.withTrimmedLeft(16).withTrimmedRight(4), juce::Justification::centredLeft, true);
  }
  // The ruler: bar numbers, a tick per beat.
  const auto ruler = rulerArea();
  const int64_t a = from(), b = to();
  g.setColour(theme::kField);
  g.fillRect(ruler);
  for (int64_t t = a / doc::kPpq * doc::kPpq; t <= b; t += doc::kPpq) {
    const int x = xOf(t);
    const bool bar = t % kBar == 0;
    g.setColour(bar ? theme::kMuted : theme::kGrid);
    g.drawVerticalLine(x, (float)ruler.getBottom() - (bar ? 12.0f : 5.0f), (float)ruler.getBottom());
    if (bar) {
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String(t / kBar + 1), x + 3, ruler.getY(), 40, ruler.getHeight() - 4, juce::Justification::centredLeft, false);
    }
  }
  // The lanes.
  const auto ls = lanes();
  const auto area = lanesArea();
  if (ls.empty()) {
    g.setColour(theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(u8("Click a track above to show it here"), area, juce::Justification::centred, false);
  }
  std::map<int64_t, int> starts;  // song tick -> lanes playing a note there (the alignment guides)
  for (const auto& l : ls) {
    const doc::Track* t = d.findTrack(l.trackId);
    const bool act = l.trackId == active_;
    const juce::Colour colour = trackColour(*t);
    auto head = l.box.withWidth(kHead);
    auto notes = l.box.withTrimmedLeft(kHead);
    g.setColour(act ? theme::kCard.brighter(0.08f) : theme::kCard);
    g.fillRect(head);
    g.setColour(colour);
    g.fillRect(head.getX() + 4, head.getY() + 4, 3, std::max(8, head.getHeight() - 8));
    g.setColour(act ? theme::kText : theme::kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    g.drawText(u8(t->name), head.withTrimmedLeft(12).withHeight(std::min(head.getHeight(), 20)), juce::Justification::centredLeft, true);
    if (head.getHeight() >= 34) {
      g.setColour(theme::kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      const std::string plugin = t->instrument.kind == "plugin" && t->instrument.plugin.is_object() ? t->instrument.plugin.value("name", std::string()) : std::string();
      g.drawText(u8(plugin.empty() ? std::string("no plugin") : plugin) + u8(" · ") + juce::String((int)l.rows.size()) +
                     (l.rows.size() == 1 ? " row" : " rows"),
                 head.withTrimmedLeft(12).withTrimmedTop(18).withHeight(16), juce::Justification::centredLeft, true);
    }
    g.setColour(act ? theme::kField.brighter(0.04f) : theme::kField);
    g.fillRect(notes);
    // Rows and the grid: bars strong, beats faint.
    g.setColour(theme::kGrid.withAlpha(0.6f));
    for (size_t i = 1; i < l.rows.size(); ++i) g.drawHorizontalLine((int)(l.box.getY() + 3 + i * l.rowH), (float)notes.getX(), (float)notes.getRight());
    for (int64_t s = a / doc::kPpq * doc::kPpq; s < b; s += doc::kPpq) {
      g.setColour(s % kBar == 0 ? theme::kOutline : theme::kGrid);
      g.drawVerticalLine(xOf(s), (float)notes.getY(), (float)notes.getBottom());
    }
    // Where the track has no clip: shaded, a click there makes one.
    if (const doc::Track* tr = d.findTrack(l.trackId)) {
      std::vector<std::pair<int64_t, int64_t>> spans;
      for (const auto& c : tr->clips) spans.push_back({c.startTick, c.startTick + c.lengthTicks});
      std::sort(spans.begin(), spans.end());
      int64_t at = a;
      g.setColour(theme::kBackground.withAlpha(0.45f));
      for (const auto& [s, e] : spans) {
        if (s > at) g.fillRect(juce::Rectangle<int>::leftTopRightBottom(xOf(at), notes.getY(), xOf(std::min(s, b)), notes.getBottom()));
        at = std::max(at, e);
      }
      if (at < b) g.fillRect(juce::Rectangle<int>::leftTopRightBottom(xOf(at), notes.getY(), notes.getRight(), notes.getBottom()));
    }
    g.saveState();
    g.reduceClipRegion(notes);
    for (const auto& n : doc::songNotesIn(d, l.trackId, a, b)) {
      const auto r = noteRect(l, n.start, l.drums ? std::min<int64_t>(n.length, kStep) : n.length, n.pitch);
      const bool sel = act && selected_.count(key(n.patternId, n.noteId));
      g.setColour(colour.withAlpha(act ? (float)(0.35 + 0.65 * n.velocity) : (float)(0.25 + 0.35 * n.velocity)));
      g.fillRoundedRectangle(r, 1.5f);
      if (sel) { g.setColour(theme::kText); g.drawRoundedRectangle(r, 1.5f, 1.2f); }
      ++starts[n.start];
    }
    g.restoreState();
    g.setColour(theme::kOutline);
    g.drawHorizontalLine(l.box.getBottom() - 1, (float)l.box.getX(), (float)l.box.getRight());
  }
  // Alignment: a thin line through every lane where the dragged note starts and another lane plays too.
  if (guideTick_ >= 0 && starts.count(guideTick_) && starts[guideTick_] > 1) {
    g.setColour(theme::kStartLine.withAlpha(0.7f));
    g.drawVerticalLine(xOf(guideTick_), (float)area.getY(), (float)area.getBottom());
  }
  // The active track's velocity.
  const auto vel = velArea();
  g.setColour(theme::kField);
  g.fillRect(vel);
  g.setColour(theme::kOutline);
  g.drawHorizontalLine(vel.getY(), 0.0f, (float)getWidth());
  g.setColour(theme::kDim);
  g.setFont(juce::FontOptions(theme::kFontSmall));
  g.drawText(u8("VELOCITY"), juce::Rectangle<int>(12, vel.getY(), kHead - 16, vel.getHeight()), juce::Justification::centredLeft, false);
  if (const doc::Track* t = d.findTrack(active_)) {
    g.setColour(trackColour(*t));
    for (const auto& n : doc::songNotesIn(d, active_, a, b)) {
      const int h = (int)std::round(n.velocity * (vel.getHeight() - 6));
      g.fillRect(xOf(n.start), vel.getBottom() - 2 - h, 3, h);
    }
  }
  // The play position.
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
  if (rulerArea().contains(p)) { host_.movePlayhead(tickAt(p.x, true)); return; }
  if (velArea().contains(p)) {
    if (active_.empty()) return;
    drag_ = Drag::Velocity;
    host_.startGesture(u8("Velocity"));
    setVelocityAt(p);
    return;
  }
  const auto ls = lanes();
  const Lane* l = laneAt(p, ls);
  if (!l) return;
  if (l->trackId != active_) {
    active_ = l->trackId;
    selected_.clear();
    if (onActiveTrack) onActiveTrack(active_);
  }
  if (p.x < kHead) { repaint(); return; }
  const std::string trackId = l->trackId;
  if (auto hit = hitAt(*l, p)) {
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
  if (e.mods.isPopupMenu()) return;
  // An empty place: a new note there (a drum hit one step long), grabbed so it can be dragged at once.
  const int pitch = pitchAt(*l, p.y);
  const int64_t at = tickAt(p.x, !e.mods.isShiftDown());
  std::pair<std::string, std::string> made;
  try {
    host_.startGesture(u8("Add note"));
    host_.applyEdit([&](doc::Document& d) { made = doc::addSongNote(d, trackId, at, kStep, pitch, 0.8); }, u8("Add note"));
  } catch (const std::exception&) {
    host_.finishGesture();
    return;
  }
  selected_ = {key(made.first, made.second)};
  grabbed_ = {made.first, made.second, at, kStep, pitch, false};
  dragTrack_ = trackId;
  grabOffset_ = tickAt(p.x, false) - at;
  drag_ = Drag::Move;
  guideTick_ = at;
  repaint();
}

void MultiTrackEditor::mouseDrag(const juce::MouseEvent& e) {
  const auto p = e.getPosition();
  if (drag_ == Drag::Velocity) { setVelocityAt(p); return; }
  if (drag_ == Drag::None) return;
  const auto ls = lanes();
  const Lane* l = nullptr;
  for (const auto& x : ls) if (x.trackId == dragTrack_) l = &x;
  if (!l) return;
  const std::string trackId = dragTrack_;
  const Hit h = grabbed_;
  if (drag_ == Drag::Move) {
    const int64_t wanted = e.mods.isShiftDown() ? tickAt(p.x, false) - grabOffset_
                                                : std::max<int64_t>(0, (tickAt(p.x, false) - grabOffset_ + kStep / 2) / kStep * kStep);
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
    const int64_t end = std::max<int64_t>(h.start + 1, (tickAt(p.x, false) + kStep / 2) / kStep * kStep);
    try {
      host_.applyEdit([&](doc::Document& d) { doc::resizeSongNote(d, trackId, h.patternId, h.noteId, end - h.start); }, u8("Length"));
    } catch (const std::exception&) {}
  }
  repaint();
}

void MultiTrackEditor::mouseUp(const juce::MouseEvent&) {
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
  const int64_t a = from(), len = to() - from();
  const double step = (std::abs(w.deltaY) > std::abs(w.deltaX) ? w.deltaY : w.deltaX);
  if (step == 0) return;
  if (e.mods.isCtrlDown()) {  // zoom: 1 to 32 bars, around the mouse
    const int64_t bars = std::clamp<int64_t>(len / kBar + (step > 0 ? -1 : 1) * std::max<int64_t>(1, len / kBar / 4), 1, 32);
    const int64_t at = tickAt(e.x, false);
    const double share = (double)(at - a) / std::max<int64_t>(1, len);
    viewLen_ = bars * kBar;
    viewFrom_ = std::max<int64_t>(0, (at - (int64_t)(share * viewLen_)) / kBar * kBar);
  } else {  // scroll a bar at a time
    viewLen_ = len;
    viewFrom_ = std::max<int64_t>(0, a + (step > 0 ? -kBar : kBar));
  }
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
