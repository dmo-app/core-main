// Client for the engine process (core-engine serve): starts it, speaks the JSON-lines protocol on its
// pipes, and reports every failure as an EngineError. No GUI and no DSP here.
// Threads: one reader (stdout), one for stderr, one writer. Nothing blocks the caller except
// command(), which waits for its own reply with a timeout; send() returns a future instead.
// Protocol: docs/contracts/engine-protocol-v1.md.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"

namespace core::client {

using json = nlohmann::json;

constexpr int kProtocolVersion = 1;

struct EngineError : std::runtime_error {
  enum class Kind {
    StartFailed,   // the process could not start, or exited before saying hello
    Timeout,       // no hello / no reply in time
    Rejected,      // the engine answered ok:false (validation, stale revision...): the session is fine
    Disconnected,  // the process is gone (closed, crashed, killed)
    Protocol       // incompatible version, malformed or too large message
  };
  Kind kind;
  EngineError(Kind k, const std::string& message) : std::runtime_error(message), kind(k) {}
};

struct EngineOptions {
  std::filesystem::path binary;
  std::vector<std::string> arguments{"serve"};
  std::chrono::milliseconds startTimeout{5000};
  std::chrono::milliseconds commandTimeout{10000};
  std::chrono::milliseconds closeTimeout{2000};  // then the process is terminated
  std::filesystem::path stderrLog;  // the engine's own messages are also appended here ("" = memory only)
};

class EngineClient {
public:
  // Events ({event: "state" | "ended" | "disconnected", ...}) arrive on the reader thread.
  using EventHandler = std::function<void(const json&)>;

  explicit EngineClient(EngineOptions options);
  ~EngineClient();
  EngineClient(const EngineClient&) = delete;
  EngineClient& operator=(const EngineClient&) = delete;

  void setEventHandler(EventHandler handler);  // before start()
  void start();                                // waits for the hello; throws EngineError
  void close();                                // ends the session; safe to call twice
  bool running() const;

  const json& hello() const { return hello_; }
  std::string sessionId() const;
  std::string diagnostics() const;  // the last lines the engine wrote to stderr

  std::future<json> send(const std::string& cmd, const json& args = json::object());
  json command(const std::string& cmd, const json& args = json::object());
  json command(const std::string& cmd, const json& args, std::chrono::milliseconds timeout);

  // Tests only: kills the process as a crash would.
  void killForTest();

private:
  struct Process;
  void readLoop();
  void errorLoop();
  void writeLoop();
  void onLine(const std::string& line);
  void disconnect(const std::string& why);
  std::future<json> enqueue(const std::string& cmd, const json& args, int64_t& id);

  EngineOptions options_;
  EventHandler handler_;
  std::unique_ptr<Process> process_;
  std::thread reader_, errors_, writer_;

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  json hello_;
  bool helloReceived_ = false, connected_ = false, stopping_ = false;
  std::string failure_;
  std::string stderrTail_;
  int64_t nextId_ = 1;
  std::map<int64_t, std::promise<json>> pending_;
  std::deque<std::string> outbox_;
  size_t outboxBytes_ = 0;
  std::atomic<size_t> maxMessageBytes_{8u << 20};
};

} // namespace core::client
