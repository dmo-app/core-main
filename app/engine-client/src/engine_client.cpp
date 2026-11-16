#include "engine_client.hpp"
#include "engine_protocol.hpp"
#include <fstream>
#ifndef _WIN32
#error "The engine client is Windows only for now (the first qualified platform)."
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace core::client {

namespace {

constexpr size_t kMaxOutboxBytes = 64u << 20;  // the engine stopped reading long before this
constexpr size_t kStderrTail = 8192;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

// Quoting for CommandLineToArgvW / the C runtime.
std::wstring quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < arg.size() && arg[i] == L'\\') { ++slashes; ++i; }
    if (i == arg.size()) { out.append(slashes * 2, L'\\'); break; }
    if (arg[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
    else { out.append(slashes, L'\\'); out += arg[i]; }
  }
  return out + L"\"";
}

std::string lastError(const char* what) { return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")"; }

void closeHandle(HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } }

} // namespace

struct EngineClient::Process {
  HANDLE process = nullptr, job = nullptr;
  HANDLE stdinWrite = nullptr, stdoutRead = nullptr, stderrRead = nullptr;
};

EngineClient::EngineClient(EngineOptions options) : options_(std::move(options)) {}
EngineClient::~EngineClient() { close(); }

void EngineClient::setEventHandler(EventHandler handler) { handler_ = std::move(handler); }

bool EngineClient::running() const { std::lock_guard<std::mutex> g(mutex_); return connected_; }
std::string EngineClient::sessionId() const { return hello_.value("sessionId", ""); }
std::string EngineClient::diagnostics() const { std::lock_guard<std::mutex> g(mutex_); return stderrTail_; }

void EngineClient::start() {
  if (process_) throw EngineError(EngineError::Kind::StartFailed, "This client was already started; use a new one.");
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
  auto closeAll = [&] { for (HANDLE* h : {&inR, &inW, &outR, &outW, &errR, &errW}) closeHandle(*h); };
  if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
    closeAll();
    throw EngineError(EngineError::Kind::StartFailed, lastError("Cannot create the engine pipes"));
  }
  for (HANDLE h : {inW, outR, errR}) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);  // our ends stay ours

  // Only these three handles reach the child, even when other clients start at the same time
  // (a stray inherited pipe end would keep another session's pipe open after its engine exits).
  HANDLE inherit[] = {inR, outW, errW};
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> attributes(size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
      !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr)) {
    closeAll();
    throw EngineError(EngineError::Kind::StartFailed, lastError("Cannot prepare the engine process"));
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = inR; si.StartupInfo.hStdOutput = outW; si.StartupInfo.hStdError = errW;
  si.lpAttributeList = list;

  const std::wstring binary = options_.binary.wstring();
  std::wstring commandLine = quote(binary);
  for (const auto& a : options_.arguments) commandLine += L" " + quote(widen(a));
  PROCESS_INFORMATION pi{};
  const BOOL created = CreateProcessW(binary.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                      &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(list);
  if (!created) {
    const std::string why = lastError(("Cannot start " + options_.binary.string()).c_str());
    closeAll();
    throw EngineError(EngineError::Kind::StartFailed, why);
  }
  // The engine dies with the app, even if the app crashes: it never keeps the audio device.
  process_ = std::make_unique<Process>();
  process_->process = pi.hProcess;
  process_->job = CreateJobObjectW(nullptr, nullptr);
  if (process_->job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(process_->job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    AssignProcessToJobObject(process_->job, pi.hProcess);
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  closeHandle(inR); closeHandle(outW); closeHandle(errW);  // the child has them now
  process_->stdinWrite = inW; process_->stdoutRead = outR; process_->stderrRead = errR;

  { std::lock_guard<std::mutex> g(mutex_); connected_ = true; }
  reader_ = std::thread([this] { readLoop(); });
  errors_ = std::thread([this] { errorLoop(); });
  writer_ = std::thread([this] { writeLoop(); });

  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait_for(lock, options_.startTimeout, [this] { return helloReceived_ || !connected_; });
  if (!helloReceived_) {
    const bool exited = !connected_;
    const std::string tail = stderrTail_;
    lock.unlock();
    close();
    if (exited) throw EngineError(EngineError::Kind::StartFailed, "The engine exited before it was ready. " + tail);
    throw EngineError(EngineError::Kind::Timeout,
                      "The engine did not answer within " + std::to_string(options_.startTimeout.count()) + " ms.");
  }
  const json protocol = hello_.value("protocol", json());
  if (protocol != kProtocolVersion) {
    lock.unlock();
    close();
    throw EngineError(EngineError::Kind::Protocol, "The engine speaks protocol " + protocol.dump() + "; this app needs " +
                                                       std::to_string(kProtocolVersion) + ".");
  }
  if (hello_.contains("limits") && hello_["limits"].value("maxMessageBytes", 0) > 0)
    maxMessageBytes_ = hello_["limits"]["maxMessageBytes"].get<size_t>();
}

void EngineClient::close() {
  if (!process_) return;
  {
    std::lock_guard<std::mutex> g(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();  // the writer sends what is queued, then closes stdin: the engine ends its session
  if (WaitForSingleObject(process_->process, (DWORD)options_.closeTimeout.count()) != WAIT_OBJECT_0)
    TerminateProcess(process_->process, 1);  // not reading or stuck: never wait forever
  if (writer_.joinable()) writer_.join();
  if (reader_.joinable()) reader_.join();
  if (errors_.joinable()) errors_.join();
  disconnect("The engine was closed.");
  closeHandle(process_->stdinWrite);
  closeHandle(process_->stdoutRead);
  closeHandle(process_->stderrRead);
  closeHandle(process_->process);
  closeHandle(process_->job);
  process_.reset();
}

void EngineClient::killForTest() {
  if (process_) TerminateProcess(process_->process, 9);
}

std::future<json> EngineClient::send(const std::string& cmd, const json& args) {
  int64_t id;
  return enqueue(cmd, args, id);
}

std::future<json> EngineClient::enqueue(const std::string& cmd, const json& args, int64_t& id) {
  std::promise<json> promise;
  std::future<json> future = promise.get_future();
  std::lock_guard<std::mutex> g(mutex_);
  if (!connected_ || stopping_) {
    promise.set_exception(std::make_exception_ptr(EngineError(EngineError::Kind::Disconnected,
        failure_.empty() ? "The engine is not running." : failure_)));
    return future;
  }
  id = nextId_++;
  std::string line = json{{"id", id}, {"cmd", cmd}, {"args", args}}.dump();
  if (line.size() > maxMessageBytes_) {
    promise.set_exception(std::make_exception_ptr(EngineError(EngineError::Kind::Protocol,
        "'" + cmd + "' is too large to send (" + std::to_string(line.size()) + " bytes, limit " + std::to_string(maxMessageBytes_.load()) + ").")));
    return future;
  }
  if (outboxBytes_ + line.size() > kMaxOutboxBytes) {
    promise.set_exception(std::make_exception_ptr(EngineError(EngineError::Kind::Protocol, "The engine is not reading its commands.")));
    return future;
  }
  line += '\n';
  outboxBytes_ += line.size();
  outbox_.push_back(std::move(line));
  pending_.emplace(id, std::move(promise));
  changed_.notify_all();
  return future;
}

json EngineClient::command(const std::string& cmd, const json& args) { return command(cmd, args, options_.commandTimeout); }

json EngineClient::command(const std::string& cmd, const json& args, std::chrono::milliseconds timeout) {
  int64_t id = -1;
  std::future<json> reply = enqueue(cmd, args, id);
  if (reply.wait_for(timeout) != std::future_status::ready) {
    std::lock_guard<std::mutex> g(mutex_);
    if (pending_.erase(id))  // a late reply is dropped
      throw EngineError(EngineError::Kind::Timeout, "No reply to '" + cmd + "' within " + std::to_string(timeout.count()) + " ms.");
  }
  return reply.get();
}

void EngineClient::writeLoop() {
  for (;;) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return !outbox_.empty() || stopping_ || !connected_; });
      if (outbox_.empty() || !connected_) break;  // stopping with nothing left, or the engine is gone
      line = std::move(outbox_.front());
      outbox_.pop_front();
      outboxBytes_ -= line.size();
    }
    const char* data = line.data();
    size_t left = line.size();
    while (left > 0) {
      DWORD written = 0;
      if (!WriteFile(process_->stdinWrite, data, (DWORD)std::min<size_t>(left, 1u << 20), &written, nullptr)) return;  // pipe closed
      data += written;
      left -= written;
    }
  }
  closeHandle(process_->stdinWrite);  // end of input: the engine finishes its session and exits
}

void EngineClient::errorLoop() {
  char buffer[4096];
  DWORD n = 0;
  std::ofstream log;
  if (!options_.stderrLog.empty()) {
    log.open(options_.stderrLog, std::ios::binary | std::ios::app);
    if (log) log << "--- engine started ---\n" << std::flush;
  }
  while (ReadFile(process_->stderrRead, buffer, sizeof buffer, &n, nullptr) && n > 0) {
    if (log) log.write(buffer, n).flush();
    std::lock_guard<std::mutex> g(mutex_);
    stderrTail_.append(buffer, n);
    if (stderrTail_.size() > kStderrTail) stderrTail_.erase(0, stderrTail_.size() - kStderrTail);
  }
}

void EngineClient::readLoop() {
  std::string partial;
  std::vector<char> buffer(1 << 16);
  DWORD n = 0;
  bool tooLarge = false;
  std::string protocolError;
  while (ReadFile(process_->stdoutRead, buffer.data(), (DWORD)buffer.size(), &n, nullptr) && n > 0) {
    partial.append(buffer.data(), n);
    size_t start = 0, end;
    while ((end = partial.find('\n', start)) != std::string::npos) {
      std::string line = partial.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) {
        // Nothing the engine writes may end this thread without the disconnect below (audit 2, bug 1):
        // pending commands fail with a clear error and the session restarts cleanly.
        try {
          onLine(line);
        } catch (const std::exception& e) {
          protocolError = e.what();
        } catch (...) {
          protocolError = "The engine's answer could not be read.";
        }
        if (!protocolError.empty()) break;
      }
      start = end + 1;
    }
    if (!protocolError.empty()) {
      TerminateProcess(process_->process, 1);
      break;
    }
    partial.erase(0, start);
    if (partial.size() > maxMessageBytes_) {  // a broken engine: end the session, never grow without limit
      tooLarge = true;
      TerminateProcess(process_->process, 1);
      break;
    }
  }
  WaitForSingleObject(process_->process, 2000);
  DWORD code = 0;
  GetExitCodeProcess(process_->process, &code);
  disconnect(!protocolError.empty() ? protocolError
             : tooLarge ? "The engine sent a message larger than the protocol allows."
                        : "The engine exited (code " + std::to_string(code) + ").");
}

void EngineClient::onLine(const std::string& line) {
  // Never throws on what the engine wrote (audit 2, bug 1). A broken protocol throws on purpose (NoId below): readLoop
  // turns that into a clean end of the session, with the reason.
  EngineLine read = readEngineLine(line);
  switch (read.kind) {
    case EngineLine::Kind::NotJson: {
      std::lock_guard<std::mutex> g(mutex_);
      stderrTail_ += "[not JSON on stdout] " + line.substr(0, 200) + "\n";
      return;
    }
    case EngineLine::Kind::Ready: {
      {
        std::lock_guard<std::mutex> g(mutex_);
        if (helloReceived_) return;
        hello_ = std::move(read.message);
        helloReceived_ = true;
      }
      changed_.notify_all();
      return;
    }
    case EngineLine::Kind::Event:
      try {
        if (handler_) handler_(read.message);
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> g(mutex_);
        stderrTail_ += std::string("[unexpected engine event] ") + e.what() + "\n";
      } catch (...) {
        std::lock_guard<std::mutex> g(mutex_);
        stderrTail_ += "[unexpected engine event] unknown handler exception\n";
      }
      return;
    case EngineLine::Kind::NoId: {
      // No command can be told its answer: end the session now instead of letting one wait for its timeout
      // (audit 2, point 4). readLoop ends the process and disconnect() fails every pending command with this.
      {
        std::lock_guard<std::mutex> g(mutex_);
        stderrTail_ += "[reply without id] " + read.error + "\n";
      }
      throw std::runtime_error(endReason(read));
    }
    case EngineLine::Kind::Reply:
      break;
  }
  std::promise<json> promise;
  {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = pending_.find(read.id);
    if (it == pending_.end()) return;  // timed out already
    promise = std::move(it->second);
    pending_.erase(it);
  }
  if (read.ok) promise.set_value(std::move(read.result));
  else promise.set_exception(std::make_exception_ptr(EngineError(EngineError::Kind::Rejected, read.error)));
}

void EngineClient::disconnect(const std::string& why) {
  std::map<int64_t, std::promise<json>> failed;
  std::string tail;
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (!connected_) return;
    connected_ = false;
    failure_ = why;
    failed.swap(pending_);
    outbox_.clear();
    outboxBytes_ = 0;
    tail = stderrTail_;
  }
  changed_.notify_all();
  for (auto& [id, promise] : failed)
    promise.set_exception(std::make_exception_ptr(EngineError(EngineError::Kind::Disconnected, why)));
  try {
    if (handler_) handler_({{"event", "disconnected"}, {"reason", why}, {"diagnostics", tail}});
  } catch (...) { /* A failed notification must not terminate the reader during shutdown. */ }
}

} // namespace core::client
