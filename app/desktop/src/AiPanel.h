// The AI panel (Diogo, 2026-11-16): a chat docked at the right side (or at the bottom), opened and closed with the
// "AI" button. Since Fase 4 it is a thin client: the AI is a plugin, its own process (core-ai, the "Core AI" package),
// and this panel only starts it, sends it what Diogo writes with the song, and shows what it answers. Everything of
// the AI lives in that process: the model and its keys, the knowledge base, the memory of recipes (SQLite, Gostei /
// Não gostei, the JSONL export) and the chats on disk. Core keeps the catalog it exposes (music creation only), the
// check of each proposal and the Undo: Apply makes a proposal Core admitted whole as ONE undo step ("AI: ...").
//
// The wire is two pipes (core_ai_link): no network on Core's side. Without the process (not installed, ended,
// misbehaving) the panel says "offline" and Core works at 100%; Settings > Restart the AI starts it again.
//
// The header: New, Chats, Copy, Delete, Settings (model, effort, what the AI may do, the memory, where the panel goes,
// the AI's state) and X (closes it; the chat stays).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ai_link.hpp"
#include "calls.hpp"
#include "catalog.hpp"

namespace core::app {

class AiPanel final : public juce::Component {
public:
  struct Host {
    std::function<const doc::Document&()> song;                 // the song as it is now
    std::function<std::vector<control::CoreFunction>()> functions;
    juce::File keyFolder;    // %APPDATA%\Core\AI: the AI's own folder (it keeps its keys, memory, chats there)
    juce::File pcoreStyles;  // pcore/styles.json beside Core.exe (the AI reads P-Core's styles from it)
    // Applies a proposal Core admits (with the zone Core sent with the request) as one undo step; returns the lines
    // applied, or throws (nothing changed then).
    std::function<std::vector<std::string>(const nlohmann::json& proposal, const control::Zone& zone)> apply;
    // The selection on the tracks (a box: tracks and bars); inactive = the whole song. The AI changes only that.
    std::function<control::Zone()> zone;
    // The plugins Core knows (its scanned list), and a scan of only some plugin files (the AI's: Settings).
    std::function<std::vector<nlohmann::json>()> instruments;
    std::function<void(const juce::Array<juce::File>&, std::function<void(int found, int failed)>)> scanPlugins;
  };
  enum class Dock { Side, Bottom };

  // The panel's own settings (ai-panel.json in the AI folder; the models and permissions stay in ai-settings.json).
  struct Prefs {
    juce::String model;            // the model's name ("" = the first, Haiku)
    int effort = 1;                // 0 low, 1 medium, 2 high
    juce::String mode;             // ask | readonly | trusted ("" = ai-settings.json's)
    bool useMemory = true;         // the recipes of earlier chats go with the requests (the memory is shared by every chat)
    bool keepMemory = true;        // what is applied becomes a recipe (Gostei / Não gostei after Apply)
    double maxUsd = 0.25;          // a message stops asking the model when it has cost this much
    Dock dock = Dock::Side;
    int width = 420, height = 320;  // at the side / at the bottom
  };

  explicit AiPanel(Host host);
  ~AiPanel() override;

  std::function<void()> onLayoutChange;
  std::function<void()> onClose;  // X

  Dock dock() const { return prefs_.dock; }
  int sideWidth() const { return prefs_.width; }
  int bottomHeight() const { return prefs_.height; }
  void setLimits(int maxWidth, int maxHeight);
  void focusInput();

  void resized() override;
  void paint(juce::Graphics& g) override;

  class Grip;
  class Settings;

private:
  // The AI process
  void startAi();
  juce::File aiExe() const;
  void received(const nlohmann::json& m);  // on the message thread
  void offline(const std::string& why);    // on the message thread
  bool online() const { return link_ && link_->online(); }
  // The chat
  void send();
  void liveText(const juce::String& piece);
  void finished(const nlohmann::json& answer);
  void say(const juce::String& who, const juce::String& text);
  void showProposal();
  void applyProposal();
  void dropProposal();
  void remember(bool liked);
  void memoryAction(int choice);  // 1 export, 2 import, 3 export for training (JSONL)
  void showFeedback(bool visible);
  void newChat();
  void deleteChat();
  void showChats();
  void showChatList(const nlohmann::json& chats, const juce::String& folder);
  void openedChat(const nlohmann::json& data);
  void showCopy();
  void showSettings();
  // Settings
  void loadPrefs();
  void savePrefs();
  void setDock(Dock d);
  void resizeBy(int start, int delta);
  int startSize() const { return prefs_.dock == Dock::Side ? prefs_.width : prefs_.height; }
  int modelIndex() const;
  ai::Permissions permissionsNow() const;  // ai-settings.json's, with the panel's mode over them

  Host host_;
  Prefs prefs_;
  std::unique_ptr<ai::Link> link_;
  juce::String aiState_;              // "online", or offline and why (Settings shows it)
  juce::StringArray models_;          // what the AI process offers (its hello)
  int recipes_ = 0;                   // in the AI's memory (its hello, and after each memory operation)
  ai::Permissions permissions_;       // ai-settings.json's (Core's own check of what the AI may do)
  std::vector<control::CoreFunction> functions_;  // the list the pending proposal was checked against (it points into it)
  juce::TextEditor transcript_, input_;
  juce::TextButton send_{"Send"}, apply_{"Apply"}, discard_{"Discard"};
  juce::TextButton new_{"New"}, chats_{"Chats"}, copy_{"Copy"}, delete_{"Delete"}, settings_{"Settings"}, close_{"X"};
  juce::TextButton like_{"Gostei"}, dislike_;
  juce::Label title_;
  std::unique_ptr<Grip> grip_;
  std::unique_ptr<juce::FileChooser> chooser_;
  int asked_ = 0;                     // messages sent (their ids)
  juce::String askId_;                // the message being answered ("" = none)
  control::Zone askZone_;             // the zone sent with it
  nlohmann::json pending_;            // the proposal waiting for Apply (null: none)
  juce::String pendingId_;            // the message it answered
  control::Zone pendingZone_;
  juce::String recipeId_;             // the recipe the AI kept for what was just applied
  juce::Label status_, cost_;
  juce::String live_;
  bool busy_ = false;
  std::vector<std::pair<juce::String, juce::String>> said_;  // what is on screen (Copy)
  int maxWidth_ = 1200, maxHeight_ = 800;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace core::app
