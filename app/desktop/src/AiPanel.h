// Core's own AI (Diogo, 2026-11-16): a chat inside the main window, docked at the right side (or at the bottom), opened
// and closed with the "AI" button at the bottom right of the window, as the chat was before (core-old ChatPanel). Its
// edge is dragged to make it wider or taller. It asks for beats, the song's structure and changes to parts; the
// answer is a proposal of Core's calls, shown here checked against the song, and Apply makes it as ONE undo step
// ("AI: ..."). Nothing else changes the song from here. Core's AI is its own: it does not go through AICORE.
//
// The header: New (a new chat), Chats (the chats kept on disk: open one again, or the folder), Copy (the last answer
// or the whole conversation; text can also be selected and copied), Delete (this chat, from the screen and the disk),
// Settings (model, effort, what the AI may do, the memory, where the panel goes) and X (closes it; the chat stays).
// Each chat is kept in %APPDATA%\Core\AI\chats after every answer, and can be taken up again with any model.
// The conversation, the tools and the cost live in core_ai (app/ai, testable); this panel only sends the requests
// (HTTPS, on a background thread) and shows the answers, the calls Core accepted or left out, and what it cost.
// The model: Claude (key ANTHROPIC_API_KEY or anthropic-key.txt in %APPDATA%\Core\AI), or any OpenAI-compatible API or
// local server listed in ai-settings.json there (ai/models.hpp).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ai_chat.hpp"
#include "models.hpp"
#include "stream.hpp"

namespace core::app {

class AiPanel final : public juce::Component {
public:
  struct Host {
    std::function<const doc::Document&()> song;                 // the song as it is now
    std::function<std::vector<control::CoreFunction>()> functions;
    const doc::PCoreCatalog* pcore = nullptr;
    const ai::Knowledge* knowledge = nullptr;                    // the style knowledge base (ai/ beside Core.exe), or none
    juce::File keyFolder;                                        // %APPDATA%\Core\AI: anthropic-key.txt, ai-settings.json
    // Applies a proposal as one undo step; returns the lines applied, or throws (nothing changed then).
    std::function<std::vector<std::string>(const nlohmann::json& proposal)> apply;
    // The selection on the tracks (a box: tracks and bars); inactive = the whole song. The AI changes only that.
    std::function<control::Zone()> zone;
    // The plugins Core knows (its scanned list), and a scan of only some plugin files (the AI's: Settings).
    std::function<std::vector<nlohmann::json>()> instruments;
    std::function<void(const juce::Array<juce::File>&, std::function<void(int found, int failed)>)> scanPlugins;
    // Renders two tracks with their plugins (ler_kick_bass: ai::Config::listen); not set = the tool is not offered.
    decltype(ai::Config::listen) listen;
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

  // The main window lays out again when the panel moves, changes size or closes.
  std::function<void()> onLayoutChange;
  std::function<void()> onClose;  // X

  Dock dock() const { return prefs_.dock; }
  int sideWidth() const { return prefs_.width; }
  int bottomHeight() const { return prefs_.height; }
  void setLimits(int maxWidth, int maxHeight);  // what the main window can give
  void focusInput();

  void resized() override;
  void paint(juce::Graphics& g) override;

  class Grip;
  class Settings;

private:
  void send();
  void post(nlohmann::json body);                  // on a background thread; the answer comes back here
  void answered(const nlohmann::json& response, const juce::String& failure);
  void liveText(const juce::String& piece);         // a piece of the answer as it streams (shown in the status line)
  void finished();
  void say(const juce::String& who, const juce::String& text);
  void showCost();
  void showProposal();
  void applyProposal();
  void dropProposal();
  void remember(bool liked);         // Gostei / Não gostei on the recipe just applied
  void memoryAction(int choice);     // 1 export, 2 import, 3 export for training (JSONL)
  void showFeedback(bool visible);
  // Chats
  void newChat();
  void saveChat();
  void openChat(const juce::File& file);
  void deleteChat();
  void showChats();
  void showCopy();
  void showSettings();
  juce::File chatsFolder() const;
  // Settings
  void loadPrefs();
  void savePrefs();
  void setDock(Dock d);
  void resizeBy(int start, int delta);  // from the grip
  int startSize() const { return prefs_.dock == Dock::Side ? prefs_.width : prefs_.height; }

  juce::String apiKey() const;
  bool needsKey() const;
  const ai::ModelEntry& entry() const;
  int modelIndex() const;
  ai::Config configNow() const;

  Host host_;
  Prefs prefs_;
  std::unique_ptr<ai::Chat> chat_;
  std::vector<ai::ModelEntry> models_;
  juce::TextEditor transcript_, input_;
  juce::TextButton send_{"Send"}, apply_{"Apply"}, discard_{"Discard"};
  juce::TextButton new_{"New"}, chats_{"Chats"}, copy_{"Copy"}, delete_{"Delete"}, settings_{"Settings"}, close_{"X"};
  juce::TextButton like_{"Gostei"}, dislike_;
  juce::Label title_;
  std::unique_ptr<Grip> grip_;
  std::unique_ptr<ai::Memory> memory_;  // %APPDATA%\Core\AI\core-ai-memory.sqlite: recipes of what Diogo applied and liked
  std::string lastRequest_, recipeId_;   // the message the proposal answered; the recipe Apply just kept
  std::unique_ptr<juce::FileChooser> chooser_;
  nlohmann::json pending_;  // the proposal waiting for Apply (null: none)
  ai::Permissions permissions_;  // from ai-settings.json; the panel's mode over it
  const ai::Memory* chatMemory_ = nullptr;  // the memory chat_ was made with (none when not shared)
  juce::Label status_, cost_;
  juce::String live_;  // what the current request has streamed so far
  bool busy_ = false;
  // The chat on screen: what was said (for Copy and the file), its file, when it began.
  std::vector<std::pair<juce::String, juce::String>> said_;
  juce::File chatFile_;
  juce::String chatTitle_;
  int maxWidth_ = 1200, maxHeight_ = 800;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace core::app
