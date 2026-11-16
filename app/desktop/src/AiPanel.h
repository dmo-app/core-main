// Core's own AI (Diogo, 2026-11-16): a chat with Claude in its own window (menu AI > AI chat). It asks for
// beats, the song's structure and changes to parts; the answer is a proposal of Core's calls, shown here checked
// against the song, and Apply makes it as ONE undo step ("AI: ..."). Nothing else changes the song from here.
// Core's AI is its own: it does not go through AICORE (Diogo, 2026-11-16).
// The conversation, the tools and the cost live in core_ai (app/ai, testable); this window only sends the requests
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
  };
  explicit AiPanel(Host host);
  ~AiPanel() override;

  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  void send();
  void post(nlohmann::json body);                  // on a background thread; the answer comes back here
  void answered(const nlohmann::json& response, const juce::String& failure);
  void finished();
  void say(const juce::String& who, const juce::String& text);
  void showCost();
  void showProposal();
  void applyProposal();
  void dropProposal();
  void remember(bool liked);         // Gostei / Não gostei on the recipe just applied
  void showMemoryMenu();             // export / import the memory
  void showFeedback(bool visible);
  juce::String apiKey() const;
  bool needsKey() const;
  const ai::ModelEntry& entry() const;
  ai::Config configNow() const;

  Host host_;
  std::unique_ptr<ai::Chat> chat_;
  std::vector<ai::ModelEntry> models_;
  juce::TextEditor transcript_, input_;
  juce::TextButton send_{"Send"}, newChat_{"New chat"}, apply_{"Apply"}, discard_{"Discard"};
  juce::TextButton like_{"Gostei"}, dislike_, memoryButton_{"Memory"};
  std::unique_ptr<ai::Memory> memory_;  // %APPDATA%\Core\AI\core-ai-memory.sqlite: recipes of what Diogo applied and liked
  std::string lastRequest_, recipeId_;   // the message the proposal answered; the recipe Apply just kept
  std::unique_ptr<juce::FileChooser> chooser_;
  nlohmann::json pending_;  // the proposal waiting for Apply (null: none)
  juce::ComboBox model_, effort_, mode_;  // mode_: the permissions' mode (ask, readonly, trusted)
  ai::Permissions permissions_;            // from ai-settings.json; mode_ changes its mode
  juce::Label status_, cost_;
  bool busy_ = false;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

class AiWindow final : public juce::DocumentWindow {
public:
  explicit AiWindow(AiPanel::Host host);
  void show();
  void closeButtonPressed() override { setVisible(false); }
};

}  // namespace core::app
