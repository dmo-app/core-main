// core-ai: Core's AI as its own process (Fase 4, 2026-11-16). Core starts it and talks to it through its stdin and
// stdout (frames, protocol.hpp); it never opens a port. It leaves when Core says bye or closes its stdin.
//
//   core-ai --data <%APPDATA%\Core\AI> --pcore <pcore/styles.json> [--knowledge <core-ai-knowledge.sqlite>]
//           [--fake-model <answers.json>]    the model's answers from a file, in order (tests; no network)
//           [--evil <steps.json>]            a scripted AI that breaks the rules on purpose (Core's tests only)
//
// The model is reached over HTTPS only in the Windows build (JUCE's URL); elsewhere only --fake-model answers.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include "ai_protocol.hpp"
#include "service.hpp"
#include "stream.hpp"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <unistd.h>
#endif
#if CORE_AI_JUCE
#include <juce_core/juce_core.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace core;

namespace {

std::mutex outLock;

void writeRaw(const std::string& bytes) {
  std::lock_guard<std::mutex> lock(outLock);
  std::fwrite(bytes.data(), 1, bytes.size(), stdout);
  std::fflush(stdout);
}

void sendToCore(const json& m) {
  const std::string f = ai::ipc::frame(m, ai::ipc::kMaxFromAi);
  if (f.empty()) {
    writeRaw(ai::ipc::frame({{"type", "error"}, {"text", "an answer was too big to send"}}, ai::ipc::kMaxFromAi));
    return;
  }
  writeRaw(f);
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

[[noreturn]] void stallForever() {
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

std::string arg(int argc, char** argv, const char* name) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == name) return argv[i + 1];
  return {};
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Reads the frames from Core on stdin. Returns false at the end of stdin.
struct Input {
  ai::ipc::Decoder decoder{ai::ipc::kMaxToAi};
  std::deque<json> ready;
  bool next(json& out) {
    char buf[1 << 16];
    while (ready.empty()) {
      // read(), not fread(): fread waits for a full buffer, a frame must be handled as soon as it is here.
#ifdef _WIN32
      const int got = _read(0, buf, (unsigned)sizeof buf);
#else
      const ssize_t got = ::read(0, buf, sizeof buf);
      if (got < 0 && errno == EINTR) continue;
#endif
      if (got <= 0) return false;
      const size_t n = (size_t)got;
      for (auto& m : decoder.feed(buf, n)) ready.push_back(std::move(m));
      if (decoder.failed()) return false;  // Core's stream cannot be trusted any more
    }
    out = std::move(ready.front());
    ready.pop_front();
    return true;
  }
};

// --evil: a scripted process for Core's tests (a misbehaving AI). Steps, in order:
//   {"send": message}            a frame with that message
//   {"rawHex": "0a000000..."}    these bytes as they are (broken frames, half frames)
//   {"waitFor": "ask"}           reads Core's messages until one of that type
//   {"stall": true}              hangs until it is ended
//   {"exit": 3}                  leaves with that code
int runEvil(const fs::path& script) {
  const json steps = json::parse(readFile(script), nullptr, false);
  Input in;
  if (!steps.is_array()) return 2;
  for (const auto& s : steps) {
    if (s.contains("send")) writeRaw(ai::ipc::frame(s["send"], size_t(1) << 30));
    if (s.contains("rawHex")) {
      const std::string hex = s["rawHex"].get<std::string>();
      std::string bytes;
      for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back((char)std::stoi(hex.substr(i, 2), nullptr, 16));
      writeRaw(bytes);
    }
    if (s.contains("waitFor")) {
      json m;
      while (in.next(m))
        if (m.value("type", std::string()) == s["waitFor"].get<std::string>()) break;
    }
    if (s.contains("stall")) stallForever();
    if (s.contains("exit")) return s["exit"].get<int>();
  }
  json m;
  while (in.next(m)) {
  }
  return 0;
}

// --fake-model: the answers in order; {"__stall": true} hangs there (the model never answers: Core's kill tests).
struct FakeModel {
  json answers = json::array();
  size_t next = 0;
  json respond(std::string* failure) {
    if (next >= answers.size()) {
      *failure = "the fake model has no more answers";
      return nullptr;
    }
    const json& a = answers[next++];
    if (a.is_object() && a.value("__stall", false)) stallForever();
    if (a.is_object() && a.contains("__fail")) {
      *failure = a["__fail"].get<std::string>();
      return nullptr;
    }
    return a;
  }
};

#if CORE_AI_JUCE
// HTTPS to the model (JUCE's URL: WinINet on Windows), streaming when the request asks for it (A8).
json httpJuce(const ai::HttpRequest& req, const std::function<void(const std::string&)>& onText,
              const std::function<void(const std::string&)>& onTool, std::string* failure) {
  const bool streaming = req.body.value("stream", false);
  const std::string data = req.body.dump(-1, ' ', false, json::error_handler_t::replace);
  juce::String headers;
  for (const auto& [k, v] : req.headers) headers += juce::String::fromUTF8(k.c_str()) + ": " + juce::String::fromUTF8(v.c_str()) + "\r\n";
  int status = 0;
  const juce::URL url = juce::URL(juce::String::fromUTF8(req.url.c_str())).withPOSTData(juce::String::fromUTF8(data.c_str()));
  const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                           .withExtraHeaders(headers.trimEnd())
                           .withConnectionTimeoutMs(180000)
                           .withStatusCode(&status);
  auto stream = url.createInputStream(options);
  if (!stream) {
    *failure = "no connection to " + req.url;
    return nullptr;
  }
  json response;
  if (streaming) {
    ai::StreamAssembler a;
    a.onText = onText;
    a.onTool = onTool;
    std::string raw;
    char buf[4096];
    for (int n; (n = stream->read(buf, (int)sizeof buf)) > 0;) {
      a.feed(std::string_view(buf, (size_t)n));
      if (!a.sawEvents() && raw.size() < (1u << 20)) raw.append(buf, (size_t)n);
    }
    a.finish();
    if (a.sawEvents()) return a.message();
    response = json::parse(raw, nullptr, false);  // not a stream: an HTTP error's JSON
  } else {
    response = json::parse(stream->readEntireStreamAsString().toStdString(), nullptr, false);
  }
  if (response.is_discarded()) {
    *failure = "the answer is not JSON (HTTP " + std::to_string(status) + ")";
    return nullptr;
  }
  return response;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (const std::string evil = arg(argc, argv, "--evil"); !evil.empty()) return runEvil(fromUtf8(evil));

  ai::Service::Env env;
  env.dataDir = fromUtf8(arg(argc, argv, "--data"));
  env.pcoreStyles = fromUtf8(arg(argc, argv, "--pcore"));
  std::string knowledge = arg(argc, argv, "--knowledge");
  if (knowledge.empty() && argc > 0) {
    const fs::path beside = fromUtf8(argv[0]).parent_path() / "ai" / "core-ai-knowledge.sqlite";
    if (fs::exists(beside)) knowledge = utf8(beside);
  }
  env.knowledgeDb = fromUtf8(knowledge);
  env.nowMs = nowMs;
  env.send = sendToCore;
  std::error_code ec;
  fs::create_directories(env.dataDir, ec);
  auto logFile = std::make_shared<std::ofstream>(env.dataDir / "core-ai.log", std::ios::app);
  env.log = [logFile](const std::string& line) { *logFile << line << std::endl; };

  auto fake = std::make_shared<FakeModel>();
  if (const std::string f = arg(argc, argv, "--fake-model"); !f.empty()) {
    fake->answers = json::parse(readFile(fromUtf8(f)), nullptr, false);
    if (!fake->answers.is_array()) fake->answers = json::array();
    env.http = [fake](const ai::HttpRequest&, const auto&, const auto&, std::string* failure) { return fake->respond(failure); };
  } else {
#if CORE_AI_JUCE
    env.http = httpJuce;
#else
    env.http = [](const ai::HttpRequest&, const auto&, const auto&, std::string* failure) -> json {
      *failure = "this build of core-ai has no network (tests only)";
      return nullptr;
    };
#endif
  }

  ai::Service service(env);
  // One worker answers Core's messages in order; this thread keeps reading, so "cancel" is seen at once.
  std::mutex lock;
  std::condition_variable wake;
  std::deque<json> queue;
  bool done = false, idle = true;
  std::thread worker([&] {
    for (;;) {
      json m;
      {
        std::unique_lock<std::mutex> l(lock);
        wake.wait(l, [&] { return done || !queue.empty(); });
        if (queue.empty()) return;
        m = std::move(queue.front());
        queue.pop_front();
        idle = false;
      }
      service.handle(m);
      {
        std::lock_guard<std::mutex> l(lock);
        idle = true;
      }
      wake.notify_all();
    }
  });
  Input in;
  json m;
  while (in.next(m)) {
    if (const std::string why = ai::ipc::checkToAi(m); !why.empty()) {
      sendToCore({{"type", "error"}, {"text", ("a message from Core was refused: " + why).substr(0, 4000)}});
      env.log("refused from Core: " + why);
      continue;
    }
    const std::string type = m.value("type", std::string());
    if (type == "bye") break;
    if (type == "cancel") {
      service.cancel();
      continue;
    }
    {
      std::lock_guard<std::mutex> l(lock);
      queue.push_back(std::move(m));
    }
    wake.notify_all();
  }
  // Core left: finish what is quick, never wait on the model.
  service.cancel();
  bool finished;
  {
    std::unique_lock<std::mutex> l(lock);
    done = true;
    wake.notify_all();
    finished = wake.wait_for(l, std::chrono::seconds(2), [&] { return queue.empty() && idle; });
  }
  if (!finished) {
    std::fflush(stdout);
    std::_Exit(0);
  }
  worker.join();
  return 0;
}
