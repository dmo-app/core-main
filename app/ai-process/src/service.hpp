// The AI's process (Fase 4, 2026-11-16): Core's AI as a plugin, outside Core.exe. Core starts it (core-ai) and talks
// to it through pipes (core_ai_link, protocol.hpp); everything of the AI lives here: the chat with the model, the
// knowledge base, the memory of recipes (SQLite; Gostei / Não gostei; the JSONL export for training), the chats kept
// on disk, the keys and the model settings. Core keeps the catalog, the check of a proposal and the Undo.
//
// The service is the process without its pipes and without HTTP, so it is tested like the rest: the messages from
// Core go to handle(), what it says goes to Env::send, and every request to the model goes to Env::http.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ai_chat.hpp"
#include "models.hpp"

namespace core::ai {

class Service {
public:
  struct Env {
    std::filesystem::path dataDir;      // %APPDATA%\Core\AI: ai-settings.json, the keys, the memory, chats\, core-ai.log
    std::filesystem::path pcoreStyles;  // pcore/styles.json (Core's, beside Core.exe)
    std::filesystem::path knowledgeDb;  // ai/core-ai-knowledge.sqlite of this package ("" = none)
    // One request to the model. onText / onTool get the answer as it streams (may never be called). Returns the
    // response, or null and why in *failure.
    std::function<nlohmann::json(const HttpRequest&, const std::function<void(const std::string&)>& onText,
                                 const std::function<void(const std::string&)>& onTool, std::string* failure)>
        http;
    std::function<void(const nlohmann::json&)> send;  // a message to Core
    std::function<int64_t()> nowMs;
    std::function<void(const std::string&)> log;      // the process's log file
  };

  explicit Service(Env env);
  ~Service();

  // One message from Core (already checked by ipc::checkToAi). Runs to the end (an ask runs every request of its
  // message); the process runs it on its worker thread, so only "cancel" needs to be called meanwhile.
  void handle(const nlohmann::json& message);
  // From any thread: the message being answered stops at the next request.
  void cancel() { cancelled_ = true; }

private:
  void hello(const nlohmann::json& m);
  void ask(const nlohmann::json& m);
  void applied(const nlohmann::json& m);
  void feedback(const nlohmann::json& m);
  void memory(const nlohmann::json& m);
  void chats(const nlohmann::json& m);
  void result(const std::string& op, bool ok, const std::string& text, const nlohmann::json& data = nullptr);
  Config configFor(const nlohmann::json& settings, std::string* problem) const;
  std::string keyFor(const ModelEntry& e) const;
  const ModelEntry& entryNamed(const std::string& name) const;
  void saveChat();
  void newChat();
  std::filesystem::path chatsDir() const;
  void note(const std::string& who, const std::string& text);

  Env env_;
  std::vector<ModelEntry> models_;
  Permissions permissions_;
  std::vector<std::string> notices_;
  std::vector<std::string> scope_;  // what Core exposes (its hello)
  std::unique_ptr<doc::PCoreCatalog> pcore_;
  std::vector<control::CoreFunction> functions_;
  std::unique_ptr<Knowledge> knowledge_;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Chat> chat_;
  bool chatShared_ = false;           // chat_ was made with the memory
  bool forceNew_ = false;             // an opened chat: the next message makes the chat with its settings
  std::atomic<bool> cancelled_{false};
  // The last answered message, for "applied": its request, the song's style, its proposal.
  std::string lastAskId_, lastRequest_, lastStyle_;
  nlohmann::json lastProposal_;
  bool keepMemory_ = true;
  // The chat on disk: what was said, its file, its title.
  std::vector<std::pair<std::string, std::string>> said_;
  std::filesystem::path chatFile_;
  std::string chatTitle_;
};

}  // namespace core::ai
