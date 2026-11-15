#include "history.hpp"

namespace core::doc {

namespace {
const std::string kNone;
size_t sizeOf(const Document& d) { return toJson(d).dump().size(); }
}

History::History(Document initial, size_t maxBytes) : current_(std::move(initial)), maxBytes_(maxBytes) { markSaved(); }

void History::push(std::deque<Step>& stack, Document document, const std::string& label) {
  const size_t bytes = sizeOf(document);
  bytes_ += bytes;
  stack.push_back({std::move(document), label, bytes});
}

// Oldest undo steps go first; the newest step always stays.
void History::trim() {
  while (bytes_ > maxBytes_ && undo_.size() > 1) {
    bytes_ -= undo_.front().bytes;
    undo_.pop_front();
  }
}

// A change that only brings back what was there before the last step of the same kind (a value moved, then moved
// back in a second gesture) takes that step away instead of adding one: the round trip leaves nothing to undo.
// Diogo 2026-11-08: "se alterar um valor e voltar ao anterior ele assume os dois" (audit part 10).
bool History::cancelsLastStep(const Document& next, const std::string& label) {
  if (undo_.empty() || undo_.back().label != label || !sameContent(undo_.back().document, next)) return false;
  bytes_ -= undo_.back().bytes;
  undo_.pop_back();
  for (const auto& s : redo_) bytes_ -= s.bytes;
  redo_.clear();
  return true;
}

void History::commit(const Document& next, const std::string& label) {
  if (gesture_) endGesture();
  if (sameContent(next, current_)) return;
  if (cancelsLastStep(next, label)) {
    current_ = next;
    ++revision_;
    return;
  }
  push(undo_, current_, label);
  for (const auto& s : redo_) bytes_ -= s.bytes;
  redo_.clear();
  current_ = next;
  ++revision_;
  trim();
}

void History::beginGesture(const std::string& label) {
  if (gesture_) endGesture();
  gesture_ = true;
  gestureStart_ = current_;
  gestureLabel_ = label;
}

void History::updateGesture(const Document& next) {
  if (!gesture_) beginGesture("");
  current_ = next;
  ++revision_;
}

void History::endGesture() {
  if (!gesture_) return;
  gesture_ = false;
  if (sameContent(gestureStart_, current_)) return;
  if (cancelsLastStep(current_, gestureLabel_)) return;
  push(undo_, std::move(gestureStart_), gestureLabel_);
  for (const auto& s : redo_) bytes_ -= s.bytes;
  redo_.clear();
  trim();
}

const std::string& History::undoLabel() const { return undo_.empty() ? kNone : undo_.back().label; }
const std::string& History::redoLabel() const { return redo_.empty() ? kNone : redo_.back().label; }

bool History::undo() {
  if (gesture_) endGesture();
  if (undo_.empty()) return false;
  Step step = std::move(undo_.back());
  undo_.pop_back();
  bytes_ -= step.bytes;
  push(redo_, std::move(current_), step.label);
  current_ = std::move(step.document);
  ++revision_;
  return true;
}

bool History::redo() {
  if (gesture_) endGesture();
  if (redo_.empty()) return false;
  Step step = std::move(redo_.back());
  redo_.pop_back();
  bytes_ -= step.bytes;
  push(undo_, std::move(current_), step.label);
  current_ = std::move(step.document);
  ++revision_;
  trim();
  return true;
}

void History::amend(const Document& next) {
  current_ = next;
  ++revision_;
}

void History::markSaved() {
  saved_ = toJson(current_);
  checkedRevision_ = revision_;
  modifiedCache_ = false;
}

bool History::modified() const {
  if (checkedRevision_ != revision_) {
    modifiedCache_ = toJson(current_) != saved_;
    checkedRevision_ = revision_;
  }
  return modifiedCache_;
}

void History::reset(Document document) {
  current_ = std::move(document);
  ++revision_;
  undo_.clear();
  redo_.clear();
  bytes_ = 0;
  gesture_ = false;
  markSaved();
}

} // namespace core::doc
