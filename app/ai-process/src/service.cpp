#include "service.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include "ai_protocol.hpp"
#include "samples.hpp"
#include "ai_scope.hpp"

namespace core::ai {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

bool writeFile(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path part = p.string() + ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;
  }
  fs::rename(part, p, ec);
  return !ec;
}

std::string trim(std::string s) {
  while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
  size_t i = 0;
  while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
  return s.substr(i);
}

// Only a plain file name of a chat in chats\ (never a path out of it).
bool plainChatName(const std::string& name) {
  return !name.empty() && name.size() < 200 && name.find_first_of("/\\:") == std::string::npos && name != "." && name != ".." &&
         name.rfind("chat-", 0) == 0 && name.size() > 5 && name.substr(name.size() - 5) == ".json";
}

constexpr const char* kEfforts[] = {"low", "medium", "high"};

}  // namespace

Service::Service(Env env) : env_(std::move(env)) {
  const json settings = json::parse(readFile(env_.dataDir / "ai-settings.json"), nullptr, false);
  std::vector<std::string> problems;
  models_ = modelEntries(settings.is_object() ? settings : json(), &problems);
  permissions_ = permissionsFrom(settings.is_object() ? settings : json(), &problems);
  for (const auto& p : problems) notices_.push_back("ai-settings.json: " + p);
  try {
    pcore_ = std::make_unique<doc::PCoreCatalog>(doc::loadPCoreCatalog(env_.pcoreStyles.string()));
  } catch (const std::exception& e) {
    notices_.push_back(std::string("P-Core's styles could not be read: ") + e.what());
  }
  if (!env_.knowledgeDb.empty()) {
    std::string why;
    knowledge_ = Knowledge::open(env_.knowledgeDb, &why);
    if (!knowledge_) notices_.push_back("The style knowledge base is not installed: the AI works without it.");
  }
  std::string why;
  memory_ = Memory::open(env_.dataDir / "core-ai-memory.sqlite", &why);
  if (!memory_) notices_.push_back("The AI's memory could not be opened: " + why);
}

Service::~Service() { saveChat(); }

void Service::handle(const json& m) {
  const std::string type = m.value("type", std::string());
  try {
    if (type == "hello") hello(m);
    else if (type == "ask") ask(m);
    else if (type == "cancel") cancel();
    else if (type == "applied") applied(m);
    else if (type == "feedback") feedback(m);
    else if (type == "memory") memory(m);
    else if (type == "chats") chats(m);
  } catch (const std::exception& e) {
    env_.send({{"type", "error"}, {"text", std::string(e.what()).substr(0, 4000)}});
  }
}

void Service::hello(const json& m) {
  // What Core exposes, cut again to the creation scope this build knows (a newer Core cannot widen it here).
  scope_.clear();
  for (const auto& s : m.value("scope", json::array()))
    if (s.is_string() && inCreationScope(s.get<std::string>())) scope_.push_back(s.get<std::string>());
  functions_.clear();
  for (auto& f : exposedFunctions(control::buildFunctions(pcore_.get())))
    if (std::find(scope_.begin(), scope_.end(), f.id) != scope_.end()) functions_.push_back(std::move(f));
  json names = json::array();
  for (const auto& e : models_) names.push_back(e.name.substr(0, 100));
  json notices = json::array();
  for (size_t i = 0; i < notices_.size() && i < 16; ++i) notices.push_back(notices_[i].substr(0, 1000));
  if (!models_.empty() && !models_.front().keyEnv.empty() && keyFor(models_.front()).empty())
    if (notices.size() < 16)
      notices.push_back(("No key for " + models_.front().name + " yet. Set " + models_.front().keyEnv + ", or put the key alone in " +
                         (env_.dataDir / (models_.front().keyFile.empty() ? "anthropic-key.txt" : models_.front().keyFile)).string() + ".")
                            .substr(0, 1000));
  env_.send({{"type", "hello"}, {"protocol", ipc::kProtocol}, {"name", "core-ai"}, {"models", names}, {"notices", notices},
             {"recipes", memory_ ? memory_->size() : 0}});
}

const ModelEntry& Service::entryNamed(const std::string& name) const {
  for (const auto& e : models_)
    if (e.name == name) return e;
  return models_.front();
}

std::string Service::keyFor(const ModelEntry& e) const {
  if (!e.keyEnv.empty())
    if (const char* v = std::getenv(e.keyEnv.c_str()); v && *v) return trim(v);
  if (e.keyFile.empty()) return {};
  return trim(readFile(env_.dataDir / e.keyFile));
}

Config Service::configFor(const json& s, std::string* problem) const {
  const ModelEntry& e = entryNamed(s.value("model", std::string()));
  Config c = e.config;
  c.provider.apiKey = keyFor(e);
  if ((!e.keyEnv.empty() || !e.keyFile.empty()) && c.provider.apiKey.empty() && problem)
    *problem = "No key for " + e.name + ": set " + (e.keyEnv.empty() ? std::string("the key") : e.keyEnv) +
               (e.keyFile.empty() ? std::string() : " or write it into " + (env_.dataDir / e.keyFile).string());
  const int effort = std::clamp(s.value("effort", 1), 0, 2);
  c.effort = kEfforts[effort];
  c.maxUsdPerMessage = std::clamp(s.value("maxUsd", 0.25), 0.01, 5.0);
  c.stream = true;
  c.permissions = permissions_;
  const std::string mode = s.value("mode", std::string());
  for (const auto& m : permissionModes())
    if (m == mode) c.permissions.mode = m;
  return c;  // no listen: hearing the mix is not Core's AI's (ai_scope.hpp)
}

void Service::note(const std::string& who, const std::string& text) {
  if (!text.empty()) said_.push_back({who, text});
}

void Service::ask(const json& m) {
  cancelled_ = false;
  const std::string id = m.value("id", std::string());
  const std::string text = m.value("text", std::string());
  const json settings = m.value("settings", json::object());
  auto answer = [&](json a) {
    a["type"] = "answer";
    a["id"] = id;
    if (!env_.send) return;
    // Too big for the wire (a huge reply): the text is cut, never the proposal.
    if (ipc::frame(a, ipc::kMaxFromAi).empty() && a.contains("reply")) a["reply"] = a["reply"].get<std::string>().substr(0, 32 * 1024);
    env_.send(a);
  };
  std::string problem;
  const Config cfg = configFor(settings, &problem);
  if (!problem.empty()) {
    answer({{"error", problem.substr(0, 4000)}, {"log", json::array()}, {"proposal", nullptr}});
    return;
  }
  doc::Document song;
  try {
    song = doc::parseDocument(m["song"]);
  } catch (const std::exception& e) {
    answer({{"error", std::string("the song could not be read: ") + e.what()}, {"log", json::array()}, {"proposal", nullptr}});
    return;
  }
  const control::Zone zone = control::zoneFrom(m);
  const bool useMemory = settings.value("useMemory", true);
  keepMemory_ = settings.value("keepMemory", true);
  Memory* shared = useMemory ? memory_.get() : nullptr;
  if (!chat_ || chat_->config().model != cfg.model || chat_->config().effort != cfg.effort || chat_->config().provider.url() != cfg.provider.url() ||
      chat_->config().permissions.mode != cfg.permissions.mode || chat_->config().provider.apiKey != cfg.provider.apiKey ||
      chat_->config().maxUsdPerMessage != cfg.maxUsdPerMessage || chatShared_ != (shared != nullptr) || forceNew_) {
    // Another model, effort, mode or memory setting: the same conversation goes on with it.
    const json kept = chat_ ? chat_->messages() : json::array();
    chat_ = std::make_unique<Chat>(cfg, functions_, pcore_.get(), knowledge_.get(), shared);
    chat_->restore(kept);
    chatShared_ = shared != nullptr;
    forceNew_ = false;
  }
  if (chatTitle_.empty()) chatTitle_ = text.substr(0, 60);
  note("Diogo", text);
  lastAskId_.clear();
  lastProposal_ = nullptr;
  json body = chat_->ask(text, song, zone.active() ? &zone : nullptr);
  std::string failure;
  while (!body.is_null()) {
    if (cancelled_) {
      chat_->abandon();
      failure = "stopped";
      break;
    }
    const HttpRequest req = chat_->http(body);
    auto onText = [&](const std::string& t) { env_.send({{"type", "partial"}, {"id", id}, {"text", t.substr(0, 4000)}}); };
    auto onTool = [&](const std::string& name) { env_.send({{"type", "partial"}, {"id", id}, {"text", ("\n[" + name + "] ").substr(0, 4000)}}); };
    std::string why;
    const json response = env_.http ? env_.http(req, onText, onTool, &why) : json();
    if (response.is_null() || response.is_discarded()) {
      chat_->abandon();  // the message never got an answer: it leaves the conversation
      failure = (why.empty() ? std::string("no answer from the model") : why) + " (the message was not kept: send it again)";
      break;
    }
    body = chat_->onResponse(response, song);
    if (!body.is_null())
      env_.send({{"type", "status"}, {"id", id}, {"text", "Working… " + std::to_string(chat_->log().size()) + " call(s) so far"}});
  }
  json log = json::array();
  for (size_t i = 0; i < chat_->log().size() && i < 128; ++i) log.push_back(chat_->log()[i].substr(0, 1000));
  std::string error = failure.empty() ? chat_->error() : failure;
  json proposal = nullptr;
  if (failure.empty()) {
    json p = chat_->proposal();
    if (p.contains("calls") && !p["calls"].empty()) {
      p.erase("auto");  // Core decides what applies by itself, never the AI
      proposal = p;
    }
  }
  note("AI", chat_->reply());
  for (const auto& l : chat_->log()) note("Core", l);
  note("Core", error);
  lastAskId_ = id;
  lastRequest_ = text;
  lastStyle_ = doc::songStyle(song);
  lastProposal_ = proposal;
  answer({{"reply", chat_->reply()},
          {"error", error.substr(0, 4000)},
          {"log", log},
          {"proposal", proposal},
          {"cost", {{"message", chat_->cost(chat_->usage())}, {"chat", chat_->cost(chat_->total())}, {"requests", chat_->usage().requests}}}});
  saveChat();
}

void Service::applied(const json& m) {
  // Core applied the proposal of message `id` (as one Undo step): a candidate recipe; Gostei confirms it.
  if (!memory_ || !chat_ || !keepMemory_ || m.value("id", std::string()) != lastAskId_ || lastProposal_.is_null()) return;
  const std::string recipe = memory_->applied(lastRequest_, lastStyle_, lastProposal_, chat_->config().model, chat_->shown(),
                                              env_.nowMs ? env_.nowMs() : 0, chat_->episode());
  lastProposal_ = nullptr;  // once
  if (!recipe.empty()) env_.send({{"type", "recipe"}, {"id", lastAskId_}, {"recipeId", recipe}});
}

void Service::feedback(const json& m) {
  if (!memory_) return result("feedback", false, "The AI's memory is not open.");
  const bool liked = m.value("liked", false);
  const bool ok = memory_->feedback(m.value("recipeId", std::string()), liked, m.value("note", std::string()), env_.nowMs ? env_.nowMs() : 0);
  result("feedback", ok, !ok ? "No such recipe." : liked ? "Kept in the AI's memory: similar requests start from it." : "Noted: the AI will not offer this again.");
}

void Service::result(const std::string& op, bool ok, const std::string& text, const json& data) {
  json r = {{"type", "result"}, {"op", op}, {"ok", ok}, {"text", text.substr(0, 60000)}};
  if (!data.is_null()) r["data"] = data;
  env_.send(r);
}

void Service::memory(const json& m) {
  const std::string op = m.value("op", std::string());
  if (!memory_) return result(op, false, "The AI's memory is not open.");
  const fs::path file = fromUtf8(m.value("file", std::string()));
  if (op == "count") return result(op, true, "", {{"recipes", memory_->size()}});
  if (op == "export" || op == "exportJsonl") {
    const std::string text = op == "exportJsonl" ? memory_->exportJsonl() : memory_->exportJson().dump(1, ' ', false, json::error_handler_t::replace);
    const bool ok = !file.empty() && writeFile(file, text);
    return result(op, ok, ok ? "Memory exported to " + utf8(file) : "Could not write " + utf8(file));
  }
  if (op == "import") {
    const json j = json::parse(readFile(file), nullptr, false);
    std::string why;
    const int n = j.is_discarded() ? -1 : memory_->importJson(j, &why);
    return result(op, n >= 0, n < 0 ? "Not imported: " + (why.empty() ? std::string("not JSON") : why) : "Imported " + std::to_string(n) + " new recipe(s).",
                  {{"recipes", memory_->size()}});
  }
  result(op, false, "unknown memory operation");
}

fs::path Service::chatsDir() const { return env_.dataDir / "chats"; }

void Service::saveChat() {
  if (said_.empty() || !chat_ || chat_->messages().empty()) return;
  if (chatFile_.empty()) {
    const std::time_t t = std::time(nullptr);
    char b[64];
    std::strftime(b, sizeof b, "chat-%Y-%m-%d-%H%M%S", std::localtime(&t));
    fs::path f = chatsDir() / (std::string(b) + ".json");
    for (int i = 2; fs::exists(f); ++i) f = chatsDir() / (std::string(b) + "-" + std::to_string(i) + ".json");
    chatFile_ = f;
  }
  json transcript = json::array();
  for (const auto& [who, text] : said_) transcript.push_back({{"who", who}, {"text", text}});
  const json j = {{"format", "core-ai-chat"}, {"version", 1}, {"title", chatTitle_}, {"updatedMs", env_.nowMs ? env_.nowMs() : 0},
                  {"model", chat_->config().model}, {"costUsd", chat_->cost(chat_->total())}, {"transcript", transcript},
                  {"messages", chat_->messages()}};
  writeFile(chatFile_, j.dump(-1, ' ', false, json::error_handler_t::replace));
}

void Service::newChat() {
  saveChat();
  chat_.reset();
  chatShared_ = false;
  chatFile_.clear();
  chatTitle_.clear();
  said_.clear();
  lastAskId_.clear();
  lastProposal_ = nullptr;
}

void Service::chats(const json& m) {
  const std::string op = m.value("op", std::string());
  const std::string name = m.value("file", std::string());
  if (op == "new") {
    newChat();
    return result(op, true, "");
  }
  if (op == "list") {
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(chatsDir(), ec))
      if (e.is_regular_file() && plainChatName(e.path().filename().string())) files.push_back({e.last_write_time(), e.path()});
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    json list = json::array();
    for (size_t i = 0; i < files.size() && i < 30; ++i) {
      const json j = json::parse(readFile(files[i].second), nullptr, false);
      list.push_back({{"file", files[i].second.filename().string()},
                      {"title", j.is_object() ? j.value("title", std::string()) : std::string()},
                      {"updatedMs", j.is_object() ? j.value("updatedMs", (int64_t)0) : (int64_t)0},
                      {"current", files[i].second == chatFile_}});
    }
    return result(op, true, "", {{"chats", list}, {"folder", utf8(chatsDir())}});
  }
  if (op == "deleteCurrent") {  // the chat on screen (Delete in the panel)
    std::error_code ec;
    if (!chatFile_.empty()) fs::remove(chatFile_, ec);
    said_.clear();
    newChat();
    return result(op, true, "");
  }
  if (!plainChatName(name)) return result(op, false, "not a chat of Core's AI");
  const fs::path file = chatsDir() / name;
  if (op == "delete") {
    if (file == chatFile_) said_.clear();  // nothing left to keep
    std::error_code ec;
    fs::remove(file, ec);
    newChat();
    return result(op, true, "");
  }
  if (op == "open") {
    const json j = json::parse(readFile(file), nullptr, false);
    if (!j.is_object() || j.value("format", std::string()) != "core-ai-chat") return result(op, false, "Not a chat of Core's AI: " + name);
    newChat();
    json transcript = json::array();
    for (const auto& t : j.value("transcript", json::array()))
      if (t.is_object()) {
        said_.push_back({t.value("who", std::string()), t.value("text", std::string())});
        transcript.push_back({{"who", said_.back().first}, {"text", said_.back().second}});
      }
    // The conversation is taken up at the next message, with whatever model is chosen then.
    chat_ = std::make_unique<Chat>(Config{}, functions_, pcore_.get(), knowledge_.get(), nullptr);
    chat_->restore(j.value("messages", json::array()));
    chatShared_ = false;
    forceNew_ = true;
    chatFile_ = file;
    chatTitle_ = j.value("title", std::string());
    return result(op, true, "", {{"transcript", transcript}, {"title", chatTitle_}});
  }
  result(op, false, "unknown chats operation");
}

}  // namespace core::ai
