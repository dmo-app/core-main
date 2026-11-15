// Undo / redo for the document: one gesture, one step (plan 3.2, rules 6-8).
// Steps are whole-document snapshots, limited by memory. Assets (audio) will be shared by reference
// when they exist, never copied per step.
#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include "document.hpp"

namespace core::doc {

class History {
public:
  explicit History(Document initial, size_t maxBytes = 64u << 20);

  const Document& current() const { return current_; }
  // Grows with every change of current() (an edit, undo, redo, another file): a cache of something made from the
  // document can tell it is still up to date without comparing documents.
  unsigned long long revision() const { return revision_; }

  // One finished change (a button, a typed value). A change that alters nothing adds no step.
  void commit(const Document& next, const std::string& label);
  // A continuous change (dragging a slider): the document follows every update, the history gets
  // one step at the end, and only if something really changed.
  void beginGesture(const std::string& label);
  void updateGesture(const Document& next);
  void endGesture();
  bool inGesture() const { return gesture_; }

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  const std::string& undoLabel() const;
  const std::string& redoLabel() const;
  bool undo();
  bool redo();

  // The "modified" flag compares the content with the last saved state, so undoing back to it
  // clears the flag. The window asks every frame (title, menus, autosave), so the answer is kept
  // until the document changes: only the first ask after a change serializes the document.
  void markSaved();
  bool modified() const;
  void reset(Document document);  // after opening another file: empty history, saved state
  // Replaces the current state without an undo step (what the plugins hold, written in before saving).
  void amend(const Document& next);
  // Every state Undo or Redo can bring back, and the current one (Codex review 2026-11-09 C4: the files of the
  // assets they name must stay on disk).
  template <class F> void forEachState(F&& f) const {
    f(current_);
    if (gesture_) f(gestureStart_);
    for (const auto& s : undo_) f(s.document);
    for (const auto& s : redo_) f(s.document);
  }

private:
  struct Step { Document document; std::string label; size_t bytes; };
  bool cancelsLastStep(const Document& next, const std::string& label);
  void push(std::deque<Step>& stack, Document document, const std::string& label);
  void trim();

  Document current_;
  std::deque<Step> undo_, redo_;
  bool gesture_ = false;
  Document gestureStart_;
  std::string gestureLabel_;
  json saved_;
  size_t maxBytes_, bytes_ = 0;
  // Counts every change to current_; modified() recomputes only when it moved.
  unsigned long long revision_ = 0;
  mutable unsigned long long checkedRevision_ = ~0ull;
  mutable bool modifiedCache_ = false;
};

} // namespace core::doc
