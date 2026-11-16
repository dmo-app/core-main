#include "ai_link.hpp"
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace core::ai {

using json = nlohmann::json;

// ------------------------------------------------------------------------------------------------- the process

#ifdef _WIN32

namespace {
std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
// One argument as CommandLineToArgvW reads it back.
std::wstring quoted(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
    if (i == a.size()) { out.append(slashes * 2, L'\\'); break; }
    if (a[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); }
    else { out.append(slashes, L'\\'); out.push_back(a[i]); }
  }
  out.push_back(L'"');
  return out;
}
}  // namespace

struct Link::Proc {
  HANDLE process = nullptr, job = nullptr;
  HANDLE toChild = nullptr, fromChild = nullptr;  // our ends
  DWORD pid = 0;
  bool spawn(const Options& o, std::string* why) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE childIn = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &toChild, &sa, 1 << 16) || !CreatePipe(&fromChild, &childOut, &sa, 1 << 16)) {
      if (why) *why = "the pipes could not be made";
      return false;
    }
    SetHandleInformation(toChild, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromChild, HANDLE_FLAG_INHERIT, 0);
    // The child inherits its two pipe ends and nothing else (not the engine's, not another child's).
    HANDLE inherit[2] = {childIn, childOut};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrBuf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childIn;
    si.StartupInfo.hStdOutput = childOut;
    si.StartupInfo.hStdError = nullptr;  // the AI keeps its own log file
    si.lpAttributeList = attrs;
    std::wstring cmd = quoted(wide(o.exe));
    for (const auto& a : o.args) cmd += L" " + quoted(wide(a));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(wide(o.exe).c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!ok) {
      if (why) *why = "the AI process could not be started (error " + std::to_string(GetLastError()) + ")";
      CloseHandle(toChild);
      CloseHandle(fromChild);
      toChild = fromChild = nullptr;
      return false;
    }
    // A job that ends the AI when Core ends, even when Core crashes.
    job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
      info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
      AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process = pi.hProcess;
    pid = pi.dwProcessId;
    return true;
  }
  bool write(const std::string& data) {
    size_t at = 0;
    while (at < data.size()) {
      DWORD n = 0;
      if (!toChild || !WriteFile(toChild, data.data() + at, (DWORD)std::min<size_t>(data.size() - at, 1 << 20), &n, nullptr) || n == 0) return false;
      at += n;
    }
    return true;
  }
  long read(char* buf, size_t size) {
    DWORD n = 0;
    if (!ReadFile(fromChild, buf, (DWORD)size, &n, nullptr)) return -1;
    return (long)n;
  }
  void closeInput() {
    if (toChild) CloseHandle(toChild);
    toChild = nullptr;
  }
  bool waitExit(int ms) { return WaitForSingleObject(process, (DWORD)ms) == WAIT_OBJECT_0; }
  void terminate() { TerminateProcess(process, 1); }
  int exitCode() {
    DWORD c = 0;
    GetExitCodeProcess(process, &c);
    return (int)c;
  }
  ~Proc() {
    closeInput();
    if (fromChild) CloseHandle(fromChild);
    if (process) CloseHandle(process);
    if (job) CloseHandle(job);
  }
};

#else

struct Link::Proc {
  pid_t pid = -1;
  int toChild = -1, fromChild = -1;
  int status = 0;
  bool reaped = false;
  bool spawn(const Options& o, std::string* why) {
    signal(SIGPIPE, SIG_IGN);  // writing to an AI that left must not end Core
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) {
      if (why) *why = "the pipes could not be made";
      return false;
    }
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[0]);
    posix_spawn_file_actions_addclose(&fa, out[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(o.exe.c_str()));
    for (const auto& a : o.args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const int rc = posix_spawn(&pid, o.exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    toChild = in[1];
    fromChild = out[0];
    if (rc != 0) {
      if (why) *why = "the AI process could not be started (" + std::to_string(rc) + ")";
      pid = -1;
      return false;
    }
    return true;
  }
  bool write(const std::string& data) {
    size_t at = 0;
    while (at < data.size()) {
      const ssize_t n = ::write(toChild, data.data() + at, data.size() - at);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      at += (size_t)n;
    }
    return true;
  }
  long read(char* buf, size_t size) {
    for (;;) {
      const ssize_t n = ::read(fromChild, buf, size);
      if (n < 0 && errno == EINTR) continue;
      return (long)n;
    }
  }
  void closeInput() {
    if (toChild >= 0) close(toChild);
    toChild = -1;
  }
  bool waitExit(int ms) {
    if (pid < 0 || reaped) return true;
    for (int waited = 0;; waited += 5) {
      const pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) { reaped = true; return true; }
      if (r < 0) { reaped = true; return true; }
      if (waited >= ms) return false;
      usleep(5000);
    }
  }
  void terminate() {
    if (pid > 0 && !reaped) ::kill(pid, SIGKILL);
  }
  int exitCode() { return reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
  ~Proc() {
    closeInput();
    if (fromChild >= 0) close(fromChild);
    if (pid > 0 && !reaped) {
      ::kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
  }
};

#endif

// ------------------------------------------------------------------------------------------------- the link

Link::Link() = default;

Link::~Link() { stop(300); }

int64_t Link::pid() const { return proc_ ? (int64_t)proc_->pid : -1; }

void Link::log(const std::string& line) {
  if (onLog) onLog(line);
}

bool Link::start(const Options& options, const json& hello, std::string* why) {
  stop(0);
  auto p = std::make_unique<Proc>();
  std::string w;
  if (!p->spawn(options, &w)) {
    if (why) *why = w;
    log("AI: " + w);
    return false;
  }
  proc_ = std::move(p);
  killedWhy_.clear();
  offlineSent_ = false;
  state_ = State::starting;
  reader_ = std::thread([this] { readLoop(); });
  dog_ = std::thread([this, ms = options.helloMs] { watchdog(ms); });
  if (!send(hello)) {
    if (why) *why = "the hello could not be sent";
    kill("the hello could not be sent");
    return false;
  }
  return true;
}

bool Link::send(const json& message) {
  if (state() == State::offline || !proc_) return false;
  const std::string f = ipc::frame(message, ipc::kMaxToAi);
  if (f.empty()) {
    log("AI: a message to the AI was too big and was not sent");
    return false;
  }
  std::lock_guard<std::mutex> lock(write_);
  return proc_->write(f);
}

void Link::kill(const std::string& why) {
  std::lock_guard<std::mutex> lock(stateLock_);
  if (!proc_) return;
  if (killedWhy_.empty()) killedWhy_ = why;
  proc_->terminate();
}

void Link::goOffline(const std::string& why) {
  {
    std::lock_guard<std::mutex> lock(stateLock_);
    state_ = State::offline;
  }
  helloSeen_.notify_all();
  if (!offlineSent_.exchange(true)) {
    log("AI: offline: " + why);
    if (onOffline) onOffline(why);
  }
}

void Link::readLoop() {
  ipc::Decoder decoder(ipc::kMaxFromAi);
  std::vector<char> buf(1 << 16);
  bool helloDone = false;
  std::string violation;
  for (;;) {
    const long n = proc_->read(buf.data(), buf.size());
    if (n <= 0) break;
    for (const json& m : decoder.feed(buf.data(), (size_t)n)) {
      std::string why = ipc::checkFromAi(m);
      const std::string type = m.value("type", std::string());
      if (why.empty() && !helloDone && type != "hello") why = "the first message must be hello";
      if (why.empty() && helloDone && type == "hello") why = "hello came twice";
      if (!why.empty()) {
        violation = why;
        break;
      }
      if (type == "hello") {
        helloDone = true;
        {
          std::lock_guard<std::mutex> lock(stateLock_);
          state_ = State::online;
        }
        helloSeen_.notify_all();
      }
      if (onMessage) onMessage(m);
    }
    if (violation.empty() && decoder.failed()) violation = decoder.error();
    if (!violation.empty()) break;
  }
  if (!violation.empty()) {
    log("AI: protocol violation, the AI process is stopped: " + violation);
    kill("the AI broke the protocol (" + violation + ")");
  }
  // The pipe closed: the process left or was ended. Reap it (end it if it lingers).
  if (!proc_->waitExit(300)) {
    proc_->terminate();
    proc_->waitExit(5000);
  }
  std::string why;
  {
    std::lock_guard<std::mutex> lock(stateLock_);
    why = killedWhy_;
  }
  if (why.empty()) why = "the AI process ended (code " + std::to_string(proc_->exitCode()) + ")";
  if (decoder.pending() > 0) log("AI: " + std::to_string(decoder.pending()) + " bytes of an unfinished message were dropped");
  goOffline(why);
}

void Link::watchdog(int helloMs) {
  std::unique_lock<std::mutex> lock(stateLock_);
  const bool said = helloSeen_.wait_for(lock, std::chrono::milliseconds(helloMs), [this] { return state_.load() != State::starting; });
  lock.unlock();
  if (!said) kill("the AI did not say hello within " + std::to_string(helloMs) + " ms");
}

void Link::stop(int graceMs) {
  if (!proc_) return;
  if (state() != State::offline) send({{"type", "bye"}});
  {
    std::lock_guard<std::mutex> lock(write_);
    proc_->closeInput();  // the end of its stdin: the AI leaves
  }
  {
    // The reader reaps the process and goes offline; give it graceMs, then end the process.
    std::unique_lock<std::mutex> lock(stateLock_);
    if (killedWhy_.empty()) killedWhy_ = "stopped";
    helloSeen_.wait_for(lock, std::chrono::milliseconds(graceMs), [this] { return state_.load() == State::offline; });
  }
  if (state() != State::offline) kill("stopped");
  if (reader_.joinable()) reader_.join();
  helloSeen_.notify_all();
  if (dog_.joinable()) dog_.join();
  proc_.reset();
}

}  // namespace core::ai
