// When to send the document to the engine again after project.load failed (audit 2, 2026-11-16, bug 2).
// Before: a timeout left the session "Diverged" and nothing was sent again until the next edit, so the engine
// went on playing an older song. Now a timeout (the engine was busy, the answer came late) is tried again on its
// own, a little later each time, with a NEW revision (the engine may have taken the old one late, and would refuse
// it a second time as stale). What cannot get better by itself (the engine refused the document, the message is
// too large) waits for the next edit or a restart, as before. No Windows and no GUI: tested on Linux.
#pragma once

namespace core::client {

class SyncRetry {
public:
  static constexpr int kMaxRetries = 5;     // 0.5 + 1 + 2 + 4 + 8 s, then wait for an edit or a restart
  static constexpr int kFirstDelayMs = 500;

  enum class After { Retry, WaitForEdit };

  // project.load failed; transient = no answer in time. delayMs: when to send again (Retry only).
  After failed(bool transient, int& delayMs) {
    delayMs = 0;
    if (!transient || failures_ >= kMaxRetries) return After::WaitForEdit;
    delayMs = kFirstDelayMs << failures_;
    ++failures_;
    return After::Retry;
  }
  void succeeded() { failures_ = 0; }
  void reset() { failures_ = 0; }  // a new edit or a new engine session: start again
  int failures() const { return failures_; }

private:
  int failures_ = 0;
};

}  // namespace core::client
