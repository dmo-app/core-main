// Core's end of the wire to its AI (Fase 4, 2026-11-16): starts the AI process (core-ai) with pipes on its stdin
// and stdout, sends it frames (ai_protocol.hpp) and hands back each message it sends, checked. Only local pipes: no
// socket, no port, no network. If the process is missing, will not start, ends, hangs before saying hello, or sends
// anything that breaks the protocol, the link goes offline (and stops the process); Core goes on without it.
//
// On Windows the process is put in a job that ends it when Core ends (even if Core crashes) and it inherits only its
// two pipes. Elsewhere it sees the end of its stdin and leaves.
//
// The callbacks run on the link's reader thread: the app moves them to its message thread. Nothing here is ever
// called from the audio thread, and the audio never waits on anything here (the engine is another process).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ai_protocol.hpp"

namespace core::ai {

class Link {
public:
  enum class State { offline, starting, online };
  struct Options {
    std::string exe;                // the AI process (UTF-8 path)
    std::vector<std::string> args;  // its arguments (UTF-8)
    int helloMs = 8000;             // it must say hello within this, or it is stopped
  };

  Link();
  ~Link();  // stops the process
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  // Set these before start.
  std::function<void(const nlohmann::json&)> onMessage;  // a message that passed checkFromAi (hello included)
  std::function<void(const std::string& why)> onOffline; // once per start: the process is gone, and why
  std::function<void(const std::string& line)> onLog;    // what Core should log (violations, the process's end)

  // Starts the process and sends it `hello` (Core's catalog). False and why when it cannot be started (offline).
  bool start(const Options& options, const nlohmann::json& hello, std::string* why = nullptr);
  // Sends a message; false when offline or when it is bigger than ipc::kMaxToAi.
  bool send(const nlohmann::json& message);
  // Says bye, waits up to graceMs for the process to leave, then ends it. Offline afterwards.
  void stop(int graceMs = 500);
  // Ends the process at once (the panel's "Restart", the tests).
  void kill(const std::string& why = "stopped");

  State state() const { return state_.load(); }
  bool online() const { return state() == State::online; }
  int64_t pid() const;

private:
  struct Proc;
  void readLoop();
  void watchdog(int helloMs);
  void goOffline(const std::string& why);
  void log(const std::string& line);

  std::unique_ptr<Proc> proc_;
  std::atomic<State> state_{State::offline};
  std::mutex write_;
  std::mutex stateLock_;
  std::condition_variable helloSeen_;
  std::string killedWhy_;
  std::atomic<bool> offlineSent_{true};
  std::thread reader_, dog_;
};

}  // namespace core::ai
