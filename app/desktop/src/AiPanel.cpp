#include "AiPanel.h"
#include "ai_admit.hpp"
#include "ai_scope.hpp"
#include "synth.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include "Language.h"
#include "Theme.h"

namespace core::app {

using json = nlohmann::json;
using namespace theme;

namespace {

juce::String u8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }

constexpr const char* kEfforts[] = {"low", "medium", "high"};
constexpr double kLimits[] = {0.05, 0.10, 0.25, 0.50, 1.00};
// The permission modes as the combo shows them (catalog.hpp: ask, readonly, trusted).
constexpr const char* kModes[] = {"Ask before changing", "Read only", "Trusted (applies by itself)"};

juce::String dollars(double usd) { return "$" + juce::String(usd, usd < 0.1 ? 4 : 2); }

}  // namespace

// The edge that is dragged: the left edge at the side, the top edge at the bottom.
class AiPanel::Grip final : public juce::Component {
public:
  explicit Grip(AiPanel& owner) : owner_(owner) {}
  void update() { setMouseCursor(owner_.dock() == Dock::Side ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor); }
  void paint(juce::Graphics& g) override {
    g.setColour(isMouseOverOrDragging() ? kPrimaryLight : kOutline);
    if (owner_.dock() == Dock::Side) g.fillRect(getLocalBounds().removeFromLeft(2));
    else g.fillRect(getLocalBounds().removeFromTop(2));
  }
  void mouseEnter(const juce::MouseEvent&) override { repaint(); }
  void mouseExit(const juce::MouseEvent&) override { repaint(); }
  void mouseDown(const juce::MouseEvent&) override { start_ = owner_.startSize(); }
  void mouseDrag(const juce::MouseEvent& e) override {
    const auto d = e.getOffsetFromDragStart();
    owner_.resizeBy(start_, owner_.dock() == Dock::Side ? -d.x : -d.y);
  }

private:
  AiPanel& owner_;
  int start_ = 0;
};

// The chat's settings (Settings in the header), in a call-out: each change is kept at once (ai-panel.json).
class AiPanel::Settings final : public juce::Component {
public:
  explicit Settings(AiPanel& owner) : owner_(owner) {
    auto label = [this](juce::Label& l, const char* text) {
      l.setText(u8(text), juce::dontSendNotification);
      l.setColour(juce::Label::textColourId, kMuted);
      addAndMakeVisible(l);
    };
    label(modelLabel_, "MODEL");
    for (int i = 0; i < owner_.models_.size(); ++i) model_.addItem(owner_.models_[i], i + 1);
    model_.setSelectedId(owner_.modelIndex() + 1, juce::dontSendNotification);
    model_.setTooltip(u8("Claude Haiku by default (about 1/10 of the price); Sonnet for big requests; more (other APIs, local models) in ai-settings.json"));
    model_.onChange = [this] { owner_.prefs_.model = model_.getText(); changed(); };
    addAndMakeVisible(model_);
    label(effortLabel_, "EFFORT");
    for (int i = 0; i < (int)std::size(kEfforts); ++i) effort_.addItem(u8(kEfforts[i]), i + 1);
    effort_.setSelectedId(owner_.prefs_.effort + 1, juce::dontSendNotification);
    effort_.setTooltip(u8("How much the model thinks: low is quicker and cheaper"));
    effort_.onChange = [this] { owner_.prefs_.effort = effort_.getSelectedId() - 1; changed(); };
    addAndMakeVisible(effort_);
    label(modeLabel_, "WHAT THE AI MAY DO");
    for (int i = 0; i < (int)std::size(kModes); ++i) mode_.addItem(u8(kModes[i]), i + 1);
    {
      const auto& modes = ai::permissionModes();
      const auto at = std::find(modes.begin(), modes.end(), owner_.permissionsNow().mode);
      mode_.setSelectedId(at == modes.end() ? 1 : (int)(at - modes.begin()) + 1, juce::dontSendNotification);
    }
    mode_.setTooltip(u8("Ask before changing the song (Apply), only read and explain, or apply by itself (still one Undo step). "
                        "Per action in ai-settings.json: \"permissions\": {\"allow\": [...], \"deny\": [...], \"ask\": [...]}"));
    mode_.onChange = [this] { owner_.prefs_.mode = u8(ai::permissionModes()[(size_t)(mode_.getSelectedId() - 1)]); changed(); };
    addAndMakeVisible(mode_);
    label(limitLabel_, "COST LIMIT PER MESSAGE");
    for (int i = 0; i < (int)std::size(kLimits); ++i) limit_.addItem("$" + juce::String(kLimits[i], 2), i + 1);
    limit_.setSelectedId(1, juce::dontSendNotification);
    for (int i = 0; i < (int)std::size(kLimits); ++i)
      if (std::abs(kLimits[i] - owner_.prefs_.maxUsd) < 1e-6) limit_.setSelectedId(i + 1, juce::dontSendNotification);
    limit_.setTooltip(u8("A message stops asking the model when it has cost this much (a Haiku message costs about $0.002)"));
    limit_.onChange = [this] { owner_.prefs_.maxUsd = kLimits[limit_.getSelectedId() - 1]; changed(); };
    addAndMakeVisible(limit_);
    label(memoryLabel_, "MEMORY");
    useMemory_.setButtonText(u8("Share the memory with this chat (recipes of earlier chats go with the requests)"));
    useMemory_.setToggleState(owner_.prefs_.useMemory, juce::dontSendNotification);
    useMemory_.onClick = [this] { owner_.prefs_.useMemory = useMemory_.getToggleState(); changed(); };
    addAndMakeVisible(useMemory_);
    keepMemory_.setButtonText(u8("Keep what is applied as recipes (Gostei / Não gostei)"));
    keepMemory_.setToggleState(owner_.prefs_.keepMemory, juce::dontSendNotification);
    keepMemory_.onClick = [this] { owner_.prefs_.keepMemory = keepMemory_.getToggleState(); changed(); };
    addAndMakeVisible(keepMemory_);
    for (auto* b : {&exportMemory_, &importMemory_, &trainMemory_, &folder_}) addAndMakeVisible(*b);
    exportMemory_.onClick = [this] { owner_.memoryAction(1); };
    importMemory_.onClick = [this] { owner_.memoryAction(2); };
    trainMemory_.onClick = [this] { owner_.memoryAction(3); };
    trainMemory_.setTooltip(u8("One line per recipe, with its whole conversation: for training a model (AICORE)"));
    folder_.onClick = [this] { owner_.host_.keyFolder.createDirectory(); owner_.host_.keyFolder.startAsProcess(); };
    label(aiLabel_, "THE AI (ITS OWN PROCESS)");
    aiStatus_.setText(owner_.aiState_, juce::dontSendNotification);
    aiStatus_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(aiStatus_);
    restart_.setTooltip(u8("Stops the AI process and starts it again (the song is not touched)"));
    restart_.onClick = [this] { owner_.startAi(); aiStatus_.setText(owner_.aiState_, juce::dontSendNotification); };
    addAndMakeVisible(restart_);
    label(placeLabel_, "PLACE");
    place_.addItem(u8("At the right side"), 1);
    place_.addItem(u8("At the bottom"), 2);
    place_.setSelectedId(owner_.prefs_.dock == Dock::Side ? 1 : 2, juce::dontSendNotification);
    place_.onChange = [this] { owner_.setDock(place_.getSelectedId() == 1 ? Dock::Side : Dock::Bottom); };
    addAndMakeVisible(place_);
    const int n = owner_.recipes_;
    count_.setText(juce::String(n) + (n == 1 ? u8(" recipe in the memory") : u8(" recipes in the memory")), juce::dontSendNotification);
    count_.setColour(juce::Label::textColourId, kMuted);
    addAndMakeVisible(count_);
    // The plugins the AI uses (Diogo, 2026-11-16): look for those only, not the whole VST3 folder.
    label(pluginsLabel_, "PLUGINS THE AI USES");
    pluginStatus_.setColour(juce::Label::textColourId, kText);
    pluginStatus_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(pluginStatus_);
    scanAi_.setTooltip(u8("Scans only the AI's plugins (quick); the rest of the plugin list stays as it is"));
    scanAi_.onClick = [this] { scanAiPlugins(); };
    addAndMakeVisible(scanAi_);
    getPlugin_.onClick = [this] {
      const auto known = owner_.host_.instruments ? owner_.host_.instruments() : std::vector<json>();
      for (const auto& p : ai::aiPlugins()) {
        bool listed = false;
        for (const auto& k : known)
          listed |= juce::String::fromUTF8(k.value("file", std::string()).c_str()).endsWithIgnoreCase(u8(p.file));
        if (!ai::findAiPlugin(p) && !listed && !p.download.empty()) juce::URL(u8(p.download)).launchInDefaultBrowser();
      }
    };
    addChildComponent(getPlugin_);
    refreshPlugins();
    setSize(440, 650);
  }

  void paint(juce::Graphics& g) override { g.fillAll(kPanel); }

  void resized() override {
    auto r = getLocalBounds().reduced(kSpacePad);
    auto row = [&](juce::Component& c, int h) { c.setBounds(r.removeFromTop(h)); r.removeFromTop(4); };
    row(modelLabel_, 18);
    row(model_, 26);
    auto two = r.removeFromTop(18);
    effortLabel_.setBounds(two.removeFromLeft(110));
    modeLabel_.setBounds(two);
    r.removeFromTop(4);
    two = r.removeFromTop(26);
    effort_.setBounds(two.removeFromLeft(100));
    two.removeFromLeft(10);
    mode_.setBounds(two);
    r.removeFromTop(6);
    auto lim = r.removeFromTop(26);
    limitLabel_.setBounds(lim.removeFromLeft(200));
    limit_.setBounds(lim.removeFromLeft(100));
    r.removeFromTop(10);
    row(memoryLabel_, 18);
    row(useMemory_, 24);
    row(keepMemory_, 24);
    row(count_, 18);
    auto buttons = r.removeFromTop(kSpaceButton);
    const int w = (buttons.getWidth() - 12) / 3;
    exportMemory_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    importMemory_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    trainMemory_.setBounds(buttons);
    r.removeFromTop(10);
    row(placeLabel_, 18);
    auto last = r.removeFromTop(26);
    place_.setBounds(last.removeFromLeft(200));
    last.removeFromLeft(10);
    folder_.setBounds(last);
    r.removeFromTop(10);
    row(aiLabel_, 18);
    auto ai = r.removeFromTop(26);
    restart_.setBounds(ai.removeFromRight(150));
    ai.removeFromRight(6);
    aiStatus_.setBounds(ai);
    r.removeFromTop(10);
    row(pluginsLabel_, 18);
    row(pluginStatus_, 88);
    auto pb = r.removeFromTop(kSpaceButton);
    scanAi_.setBounds(pb.removeFromLeft(190));
    pb.removeFromLeft(6);
    getPlugin_.setBounds(pb.removeFromLeft(170));
  }

private:
  void changed() { owner_.savePrefs(); }

  // Each plugin of the AI: on this computer or not, and in Core's scanned list or not.
  void refreshPlugins() {
    juce::StringArray lines;
    bool missing = false;
    const auto known = owner_.host_.instruments ? owner_.host_.instruments() : std::vector<json>();
    for (const auto& p : ai::aiPlugins()) {
      bool listed = false;
      for (const auto& k : known)
        listed |= juce::String::fromUTF8(k.value("name", std::string()).c_str()).equalsIgnoreCase(u8(p.name)) ||
                  juce::String::fromUTF8(k.value("file", std::string()).c_str()).endsWithIgnoreCase(u8(p.file));
      const bool file = ai::findAiPlugin(p).has_value() || listed;  // in a VST3 folder, or where Core's list found it
      missing |= !file;
      lines.add(u8(p.name) + u8(" (") + u8(p.licence) + u8("): ") +
                (!file ? (p.download.empty() ? u8("not installed (comes with Core's installer)") : u8("not installed (free at ") + u8(p.download) + ")")
                       : listed ? u8("installed and in Core's list") : u8("installed, not scanned yet")));
    }
    pluginStatus_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
    getPlugin_.setButtonText(u8("Get the missing ones"));
    getPlugin_.setVisible(missing);
  }

  void scanAiPlugins() {
    juce::Array<juce::File> files;
    const auto known = owner_.host_.instruments ? owner_.host_.instruments() : std::vector<json>();
    for (const auto& p : ai::aiPlugins()) {
      if (const auto f = ai::findAiPlugin(p)) {
        files.add(juce::File(juce::String(f->wstring().c_str())));
        continue;
      }
      for (const auto& k : known) {  // not in a standard folder: where Core's list found it (D:\Core-deps\vst3...)
        const juce::String at = juce::String::fromUTF8(k.value("file", std::string()).c_str());
        if (at.endsWithIgnoreCase(u8(p.file)) && juce::File::isAbsolutePath(at)) {
          files.addIfNotAlreadyThere(juce::File(at));
          break;
        }
      }
    }
    if (files.isEmpty() || !owner_.host_.scanPlugins) {
      refreshPlugins();
      return;
    }
    scanAi_.setEnabled(false);
    pluginStatus_.setText(u8("Scanning the AI's plugins…"), juce::dontSendNotification);
    juce::Component::SafePointer<Settings> self(this);
    owner_.host_.scanPlugins(files, [self](int, int failed) {
      if (self == nullptr) return;
      self->scanAi_.setEnabled(true);
      self->refreshPlugins();
      if (failed) self->pluginStatus_.setText(self->pluginStatus_.getText() + u8(" (a plugin failed its check: Plugins menu)"), juce::dontSendNotification);
    });
  }

  AiPanel& owner_;
  juce::Label modelLabel_, effortLabel_, modeLabel_, limitLabel_, memoryLabel_, placeLabel_, count_;
  juce::ComboBox model_, effort_, mode_, place_, limit_;
  juce::ToggleButton useMemory_, keepMemory_;
  juce::TextButton exportMemory_{"Export…"}, importMemory_{"Import…"}, trainMemory_{"For training…"}, folder_{"Open the AI folder"};
  juce::Label pluginsLabel_, pluginStatus_, aiLabel_, aiStatus_;
  juce::TextButton restart_{"Restart the AI"};
  juce::TextButton scanAi_{"Scan the AI's plugins"}, getPlugin_;
};

AiPanel::AiPanel(Host host) : host_(std::move(host)) {
  title_.setText(u8("AI"), juce::dontSendNotification);
  title_.setFont(juce::FontOptions(kFontTitle, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, kTitle);
  addAndMakeVisible(title_);
  grip_ = std::make_unique<Grip>(*this);
  addAndMakeVisible(*grip_);
  transcript_.setMultiLine(true, true);
  transcript_.setReadOnly(true);
  transcript_.setScrollbarsShown(true);
  transcript_.setCaretVisible(false);
  addAndMakeVisible(transcript_);
  input_.setMultiLine(true, true);
  input_.setReturnKeyStartsNewLine(false);
  input_.setTextToShowWhenEmpty(u8("Ask for a beat, a song structure or a change to a part (a box on the tracks limits it to there)…"), kText.withAlpha(0.4f));
  input_.onReturnKey = [this] { send(); };
  addAndMakeVisible(input_);
  send_.onClick = [this] { send(); };
  addAndMakeVisible(send_);
  new_.setTooltip(u8("A new chat (this one stays in Chats)"));
  new_.onClick = [this] { newChat(); };
  chats_.setTooltip(u8("The chats kept on this computer: open one again"));
  chats_.onClick = [this] { showChats(); };
  copy_.setTooltip(u8("Copy the last answer or the whole conversation (text can also be selected and copied)"));
  copy_.onClick = [this] { showCopy(); };
  delete_.setTooltip(u8("Delete this chat (from the screen and from Chats)"));
  delete_.onClick = [this] { deleteChat(); };
  settings_.setTooltip(u8("Model, effort, what the AI may do, the memory, where this panel goes, the AI's state"));
  settings_.onClick = [this] { showSettings(); };
  close_.setTooltip(u8("Close (the chat stays: the AI button opens it again)"));
  close_.onClick = [this] { if (onClose) onClose(); };
  for (auto* b : {&new_, &chats_, &copy_, &delete_, &settings_, &close_}) addAndMakeVisible(*b);
  apply_.onClick = [this] { applyProposal(); };
  apply_.setTooltip(u8("Makes the proposal on the song as one step of Undo"));
  discard_.onClick = [this] { dropProposal(); };
  addChildComponent(apply_);
  addChildComponent(discard_);
  dislike_.setButtonText(u8("Não gostei"));
  like_.setTooltip(u8("Keeps this as a recipe the AI reuses for similar requests (with any model)"));
  dislike_.setTooltip(u8("The AI will not offer this recipe again"));
  like_.onClick = [this] { remember(true); };
  dislike_.onClick = [this] { remember(false); };
  addChildComponent(like_);
  addChildComponent(dislike_);
  {
    // Core's own check of what the AI may do (the same file the AI reads to know what to offer).
    const juce::File settings = host_.keyFolder.getChildFile("ai-settings.json");
    const json j = settings.existsAsFile() ? json::parse(settings.loadFileAsString().toStdString(), nullptr, false) : json();
    std::vector<std::string> problems;
    permissions_ = ai::permissionsFrom(j.is_object() ? j : json(), &problems);
    for (const auto& p : problems) say(u8("Core"), u8("ai-settings.json: ") + u8(p));
  }
  loadPrefs();
  status_.setColour(juce::Label::textColourId, kText.withAlpha(0.7f));
  addAndMakeVisible(status_);
  cost_.setJustificationType(juce::Justification::centredRight);
  cost_.setColour(juce::Label::textColourId, kText.withAlpha(0.7f));
  addAndMakeVisible(cost_);
  startAi();
  said_.clear();  // the notices above are not part of a chat
  grip_->update();
  setSize(prefs_.width, 600);
}

AiPanel::~AiPanel() {
  alive_->store(false);
  if (link_) link_->stop(500);  // bye: the AI keeps the chat on its disk and leaves
}

// ---- the AI process

// Where the "Core AI" package is: CORE_AI_EXE, else where its installer puts it, else a "Core AI" folder beside Core's.
juce::File AiPanel::aiExe() const {
  if (const char* env = std::getenv("CORE_AI_EXE"); env && *env) return juce::File(juce::String::fromUTF8(env));
  const juce::File installed = juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
                                   .getChildFile("Programs").getChildFile("Core AI").getChildFile("core-ai.exe");
  if (installed.existsAsFile()) return installed;
  const juce::File core = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
  return core.getParentDirectory().getChildFile("Core AI").getChildFile("core-ai.exe");
}

void AiPanel::startAi() {
  if (link_) link_->stop(300);
  link_ = std::make_unique<ai::Link>();
  models_.clear();
  const juce::File exe = aiExe();
  if (!exe.existsAsFile()) {
    aiState_ = u8("offline: the AI plugin (Core AI) is not installed");
    say(u8("Core"), u8("The AI plugin (Core AI) is not installed, so the AI is offline. Core works without it."));
    status_.setText(u8("AI offline"), juce::dontSendNotification);
    return;
  }
  // The callbacks come on the link's thread: each goes to the message thread, and only while the panel lives.
  auto alive = alive_;
  link_->onMessage = [this, alive](const json& m) {
    juce::MessageManager::callAsync([this, alive, m] {
      if (alive->load()) received(m);
    });
  };
  link_->onOffline = [this, alive](const std::string& why) {
    juce::MessageManager::callAsync([this, alive, why] {
      if (alive->load()) offline(why);
    });
  };
  link_->onLog = [](const std::string& line) { juce::Logger::writeToLog(u8(line)); };
  // What Core exposes to the AI: the music-creation actions only (ai_scope.hpp), by name.
  json catalog = json::array();
  for (const auto& a : ai::catalog(ai::exposedFunctions(host_.functions()), false)) catalog.push_back(a.name);
  const json hello = {{"type", "hello"}, {"protocol", ai::ipc::kProtocol}, {"catalog", catalog}, {"scope", ai::creationScope()}};
  ai::Link::Options o;
  o.exe = exe.getFullPathName().toStdString();
  o.args = {"--data", host_.keyFolder.getFullPathName().toStdString(), "--pcore", host_.pcoreStyles.getFullPathName().toStdString()};
  host_.keyFolder.createDirectory();
  std::string why;
  aiState_ = u8("starting…");
  status_.setText(u8("Starting the AI…"), juce::dontSendNotification);
  if (!link_->start(o, hello, &why)) offline(why);
}

void AiPanel::offline(const std::string& why) {
  aiState_ = u8("offline: ") + u8(why);
  if (busy_) {
    busy_ = false;
    send_.setEnabled(true);
    say(u8("Core"), u8("The AI stopped before answering (") + u8(why) + u8("). Nothing was applied; the song is as it was."));
  }
  askId_.clear();
  live_.clear();
  status_.setText(u8("AI offline (Settings > Restart the AI)"), juce::dontSendNotification);
}

void AiPanel::received(const json& m) {
  const std::string type = m.value("type", std::string());
  if (type == "hello") {
    aiState_ = u8("online");
    models_.clear();
    for (const auto& n : m.value("models", json::array())) models_.add(u8(n.get<std::string>()));
    recipes_ = m.value("recipes", 0);
    for (const auto& n : m.value("notices", json::array())) say(u8("AI"), u8(n.get<std::string>()));
    if (!busy_) status_.setText({}, juce::dontSendNotification);
    return;
  }
  const juce::String id = u8(m.value("id", std::string()));
  if (type == "status" || type == "partial") {
    if (!busy_ || id != askId_) return;
    if (type == "partial") liveText(u8(m.value("text", std::string())));
    else if (live_.isEmpty()) status_.setText(u8(m.value("text", std::string())), juce::dontSendNotification);
    return;
  }
  if (type == "answer") {
    if (id == askId_) finished(m);
    return;
  }
  if (type == "recipe") {
    if (id == pendingId_ || pending_.is_null()) {
      recipeId_ = u8(m.value("recipeId", std::string()));
      showFeedback(recipeId_.isNotEmpty());
    }
    return;
  }
  if (type == "result") {
    const std::string op = m.value("op", std::string());
    const json data = m.value("data", json::object());
    if (data.is_object() && data.contains("recipes") && data["recipes"].is_number_integer()) recipes_ = data["recipes"].get<int>();
    if (op == "list" && m.value("ok", false)) return showChatList(data.value("chats", json::array()), u8(data.value("folder", std::string())));
    if (op == "open" && m.value("ok", false)) return openedChat(data);
    if (const std::string text = m.value("text", std::string()); !text.empty()) say(u8("Core"), u8(text));
    return;
  }
  if (type == "error") say(u8("AI"), u8(m.value("text", std::string())));
}

ai::Permissions AiPanel::permissionsNow() const {
  ai::Permissions p = permissions_;
  for (const auto& m : ai::permissionModes())
    if (u8(m) == prefs_.mode) p.mode = m;
  return p;
}

// ---- the chat

void AiPanel::setLimits(int maxWidth, int maxHeight) {
  maxWidth_ = std::max(320, maxWidth);
  maxHeight_ = std::max(220, maxHeight);
}

void AiPanel::focusInput() {
  if (isShowing()) input_.grabKeyboardFocus();
}

void AiPanel::resizeBy(int start, int delta) {
  if (prefs_.dock == Dock::Side) prefs_.width = juce::jlimit(320, maxWidth_, start + delta);
  else prefs_.height = juce::jlimit(220, maxHeight_, start + delta);
  if (onLayoutChange) onLayoutChange();
  savePrefs();
}

void AiPanel::setDock(Dock d) {
  prefs_.dock = d;
  grip_->update();
  savePrefs();
  if (onLayoutChange) onLayoutChange();
}

void AiPanel::loadPrefs() {
  const juce::File f = host_.keyFolder.getChildFile("ai-panel.json");
  const json j = f.existsAsFile() ? json::parse(f.loadFileAsString().toStdString(), nullptr, false) : json();
  if (!j.is_object()) return;
  prefs_.model = u8(j.value("model", std::string()));
  prefs_.effort = juce::jlimit(0, 2, j.value("effort", 1));
  prefs_.mode = u8(j.value("mode", std::string()));
  prefs_.useMemory = j.value("useMemory", true);
  prefs_.keepMemory = j.value("keepMemory", true);
  prefs_.maxUsd = juce::jlimit(0.01, 5.0, j.value("maxUsd", 0.25));
  prefs_.dock = j.value("dock", std::string("side")) == "bottom" ? Dock::Bottom : Dock::Side;
  prefs_.width = juce::jlimit(320, 2000, j.value("width", 420));
  prefs_.height = juce::jlimit(220, 2000, j.value("height", 320));
}

void AiPanel::savePrefs() {
  const json j = {{"model", prefs_.model.toStdString()}, {"effort", prefs_.effort}, {"mode", prefs_.mode.toStdString()},
                  {"useMemory", prefs_.useMemory}, {"keepMemory", prefs_.keepMemory}, {"maxUsd", prefs_.maxUsd},
                  {"dock", prefs_.dock == Dock::Side ? "side" : "bottom"}, {"width", prefs_.width}, {"height", prefs_.height}};
  host_.keyFolder.createDirectory();
  host_.keyFolder.getChildFile("ai-panel.json").replaceWithText(u8(j.dump(1)));
}

int AiPanel::modelIndex() const {
  const int i = models_.indexOf(prefs_.model);
  return i < 0 ? 0 : i;
}

void AiPanel::say(const juce::String& who, const juce::String& text) {
  if (text.isEmpty()) return;
  said_.push_back({who, text});
  transcript_.moveCaretToEnd();
  transcript_.insertTextAtCaret((transcript_.isEmpty() ? juce::String() : juce::String("\n\n")) + who + ": " + text);
}

void AiPanel::send() {
  const juce::String text = input_.getText().trim();
  if (busy_ || text.isEmpty()) return;
  if (!online()) {
    say(u8("Core"), u8("The AI is ") + aiState_ + u8(". Core works without it; Settings > Restart the AI tries again."));
    return;
  }
  dropProposal();  // a new message: the old proposal goes
  showFeedback(false);
  const control::Zone zone = host_.zone ? host_.zone() : control::Zone{};
  askId_ = "m" + juce::String(++asked_);
  askZone_ = zone;
  const json settings = {{"model", models_.isEmpty() ? std::string() : models_[modelIndex()].toStdString()},
                         {"effort", prefs_.effort},
                         {"mode", permissionsNow().mode},
                         {"maxUsd", prefs_.maxUsd},
                         {"useMemory", prefs_.useMemory},
                         {"keepMemory", prefs_.keepMemory}};
  const json ask = {{"type", "ask"}, {"id", askId_.toStdString()}, {"text", text.toStdString()}, {"song", doc::toJson(host_.song())},
                    {"zone", zone.active() ? zone.toJson() : json()}, {"settings", settings}};
  if (!link_->send(ask)) {
    say(u8("Core"), u8("The message could not be sent to the AI (the song is too big, or the AI just stopped)."));
    askId_.clear();
    return;
  }
  input_.clear();
  juce::String shown = text;
  if (zone.active()) {
    const int64_t bar = 4 * doc::kPpq;
    shown += u8("\n(selection: ") + juce::String((int)zone.tracks.size()) + (zone.tracks.size() == 1 ? u8(" track, bars ") : u8(" tracks, bars ")) +
             juce::String((int)(zone.startTick / bar + 1)) + "-" + juce::String((int)(zone.endTick / bar)) + ")";
  }
  say(u8("Diogo"), shown);
  busy_ = true;
  send_.setEnabled(false);
  live_.clear();
  status_.setText(u8("Thinking…"), juce::dontSendNotification);
}

void AiPanel::liveText(const juce::String& piece) {
  if (!busy_) return;
  live_ += piece;
  juce::String shown = live_.replace("\n", " ").trim();
  if (shown.length() > 160) shown = juce::String::fromUTF8("\xe2\x80\xa6") + shown.getLastCharacters(160);
  status_.setText(shown, juce::dontSendNotification);
}

void AiPanel::finished(const json& answer) {
  busy_ = false;
  send_.setEnabled(true);
  status_.setText({}, juce::dontSendNotification);
  const juce::String id = askId_;
  askId_.clear();
  say(u8("AI"), u8(answer.value("reply", std::string())));
  for (const auto& line : answer.value("log", json::array())) say(u8("Core"), u8(line.get<std::string>()));
  say(u8("Core"), u8(answer.value("error", std::string())));
  if (answer.contains("cost") && answer["cost"].is_object()) {
    const json& c = answer["cost"];
    cost_.setText(u8("This message ") + dollars(c.value("message", 0.0)) + " (" + juce::String(c.value("requests", 0)) + u8(" requests) · chat ") +
                      dollars(c.value("chat", 0.0)),
                  juce::dontSendNotification);
  }
  if (answer.contains("proposal") && answer["proposal"].is_object()) {
    pending_ = answer["proposal"];
    pendingId_ = id;
    pendingZone_ = askZone_;
    showProposal();
  }
}

// The proposal as Core admits it (ai_admit.hpp): whole, or not at all, with why.
void AiPanel::showProposal() {
  functions_ = host_.functions();
  const ai::Admission a = ai::admitProposal(pending_, functions_, host_.song(), nullptr, permissionsNow(), pendingZone_);
  if (!a.ok) {
    say(u8("Core"), u8("Core refused the AI's proposal, nothing will change: ") + u8(a.why));
    pending_ = nullptr;
    apply_.setVisible(false);
    discard_.setVisible(false);
    resized();
    return;
  }
  const control::Proposal& p = a.proposal;
  juce::String text = u8("Proposal") + (p.summary.empty() ? juce::String() : ": " + u8(p.summary));
  for (const auto& c : p.calls) text += "\n  • " + u8(control::describeCall(c));
  for (const auto& w : p.warnings()) text += "\n  ! " + u8(w);
  say(u8("Core"), text);
  apply_.setVisible(true);
  discard_.setVisible(true);
  resized();
  // Trusted (Core's setting, never the AI's word): every call allowed applies by itself, still one Undo step.
  bool automatic = permissionsNow().mode == "trusted";
  for (const auto& c : p.calls) {
    ai::Action act;
    act.name = ai::toolName(c.fn->id);
    act.function = c.fn->id;
    act.family = c.fn->family;
    automatic = automatic && ai::decide(permissionsNow(), act) == ai::Verdict::allow;
  }
  if (automatic) applyProposal();
}

void AiPanel::applyProposal() {
  if (pending_.is_null() || !host_.apply) return;
  try {
    const auto lines = host_.apply(pending_, pendingZone_);
    juce::String text = u8("Applied (Edit > Undo takes it back):");
    for (const auto& l : lines) text += "\n  • " + u8(l);
    say(u8("Core"), text);
    if (prefs_.keepMemory && online()) link_->send({{"type", "applied"}, {"id", pendingId_.toStdString()}});  // the AI keeps a recipe
  } catch (const std::exception& e) {
    say(u8("Core"), u8("Not applied, nothing changed: ") + u8(e.what()));
    return;
  }
  dropProposal();
}

void AiPanel::showFeedback(bool visible) {
  if (!visible) recipeId_.clear();
  like_.setVisible(visible);
  dislike_.setVisible(visible);
  resized();
}

void AiPanel::remember(bool liked) {
  if (recipeId_.isEmpty() || !online()) return;
  link_->send({{"type", "feedback"}, {"recipeId", recipeId_.toStdString()}, {"liked", liked}});
  showFeedback(false);
}

void AiPanel::memoryAction(int choice) {
  if (choice < 1 || choice > 3) return;
  if (!online()) {
    say(u8("Core"), u8("The AI's memory is in the AI, which is ") + aiState_ + ".");
    return;
  }
  const bool exporting = choice != 2, lines = choice == 3;
  const char* name = lines ? "core-ai-memory.jsonl" : "core-ai-memory.json";
  chooser_ = std::make_unique<juce::FileChooser>(exporting ? u8("Export the AI's memory") : u8("Import an AI memory"),
                                                 juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(name),
                                                 lines ? "*.jsonl" : "*.json");
  const int flags = exporting ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode;
  chooser_->launchAsync(flags | juce::FileBrowserComponent::canSelectFiles, [this, alive = alive_, exporting, lines](const juce::FileChooser& fc) {
    if (!alive->load() || fc.getResult() == juce::File() || !online()) return;
    const std::string op = !exporting ? "import" : lines ? "exportJsonl" : "export";
    link_->send({{"type", "memory"}, {"op", op}, {"file", fc.getResult().getFullPathName().toStdString()}});
  });
}

// ---- chats: the AI keeps them (in %APPDATA%\Core\AI\chats); the panel asks for them

void AiPanel::newChat() {
  if (busy_) return;
  if (online()) link_->send({{"type", "chats"}, {"op", "new"}});
  said_.clear();
  transcript_.clear();
  dropProposal();
  showFeedback(false);
  status_.setText({}, juce::dontSendNotification);
  cost_.setText({}, juce::dontSendNotification);
  focusInput();
}

void AiPanel::deleteChat() {
  if (busy_ || said_.empty()) return;
  juce::Component::SafePointer<AiPanel> safe(this);
  juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
                                   .withTitle(u8("Delete this chat"))
                                   .withMessage(u8("The conversation leaves the screen and Chats. The AI's memory keeps its recipes."))
                                   .withButton(u8("Delete"), 1).withButton(u8("Cancel"), 0),
                               [safe](int result) {
                                 if (result != 1 || safe == nullptr) return;
                                 if (safe->online()) safe->link_->send({{"type", "chats"}, {"op", "deleteCurrent"}});
                                 safe->said_.clear();
                                 safe->transcript_.clear();
                                 safe->dropProposal();
                               });
}

void AiPanel::showChats() {
  if (!online()) {
    say(u8("Core"), u8("The chats are kept by the AI, which is ") + aiState_ + ".");
    return;
  }
  link_->send({{"type", "chats"}, {"op", "list"}});  // the menu opens when the list comes back
}

void AiPanel::showChatList(const json& chats, const juce::String& folder) {
  juce::PopupMenu menu;
  menu.addSectionHeader(u8("EARLIER CHATS"));
  std::vector<std::string> files;
  for (const auto& c : chats) {
    if (!c.is_object()) continue;
    files.push_back(c.value("file", std::string()));
    juce::String title = u8(c.value("title", std::string()));
    if (title.isEmpty()) title = u8(files.back());
    const juce::Time when((juce::int64)c.value("updatedMs", (int64_t)0));
    menu.addItem(juce::PopupMenu::Item(when.formatted("%d/%m %H:%M") + "  " + title).setID((int)files.size()).setTicked(c.value("current", false)));
  }
  if (files.empty()) menu.addItem(juce::PopupMenu::Item(u8("No chats yet")).setEnabled(false));
  menu.addSeparator();
  menu.addItem(juce::PopupMenu::Item(u8("Open the chats folder")).setID(1000));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&chats_), [this, alive = alive_, files, folder](int id) {
    if (!alive->load() || id == 0) return;
    if (id == 1000) {
      const juce::File f(folder);
      f.createDirectory();
      f.startAsProcess();
      return;
    }
    if (id - 1 < (int)files.size() && online() && !busy_) link_->send({{"type", "chats"}, {"op", "open"}, {"file", files[(size_t)(id - 1)]}});
  });
}

void AiPanel::openedChat(const json& data) {
  said_.clear();
  transcript_.clear();
  dropProposal();
  showFeedback(false);
  for (const auto& t : data.value("transcript", json::array()))
    if (t.is_object()) say(u8(t.value("who", std::string())), u8(t.value("text", std::string())));
  focusInput();
}

void AiPanel::showCopy() {
  juce::String answer, all;
  for (const auto& [who, text] : said_) {
    if (who == "AI") answer = text;
    all += (all.isEmpty() ? juce::String() : juce::String("\n\n")) + who + ": " + text;
  }
  juce::PopupMenu menu;
  menu.addItem(1, u8("Copy the last answer"), answer.isNotEmpty());
  menu.addItem(2, u8("Copy the conversation"), all.isNotEmpty());
  menu.addItem(3, u8("Copy the selected text"), transcript_.getHighlightedText().isNotEmpty());
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&copy_), [this, alive = alive_, answer, all](int id) {
    if (!alive->load()) return;
    if (id == 1) juce::SystemClipboard::copyTextToClipboard(answer);
    if (id == 2) juce::SystemClipboard::copyTextToClipboard(all);
    if (id == 3) juce::SystemClipboard::copyTextToClipboard(transcript_.getHighlightedText());
    if (id > 0) status_.setText(u8("Copied"), juce::dontSendNotification);
  });
}

void AiPanel::showSettings() {
  if (online()) link_->send({{"type", "memory"}, {"op", "count"}});  // the count is fresh next time
  juce::CallOutBox::launchAsynchronously(std::make_unique<Settings>(*this), settings_.getScreenBounds(), nullptr);
}

void AiPanel::dropProposal() {
  pending_ = nullptr;
  pendingId_.clear();
  apply_.setVisible(false);
  discard_.setVisible(false);
  resized();
}

void AiPanel::paint(juce::Graphics& g) {
  g.fillAll(kPanel);
  g.setColour(kOutline);
  g.drawHorizontalLine(kSpaceHeader, 0.0f, (float)getWidth());
}

void AiPanel::resized() {
  auto r = getLocalBounds();
  grip_->setBounds(prefs_.dock == Dock::Side ? r.removeFromLeft(6) : r.removeFromTop(6));
  r = r.reduced(8, 0);
  auto header = r.removeFromTop(kSpaceHeader - (prefs_.dock == Dock::Side ? 0 : 6));
  close_.setBounds(header.removeFromRight(28).reduced(0, 3));  // the X always at the right
  header.removeFromRight(4);
  title_.setBounds(header.removeFromLeft(36));
  for (auto* b : {&new_, &chats_, &copy_, &delete_, &settings_}) {
    const int w = juce::GlyphArrangement::getStringWidthInt(juce::FontOptions(kFontBody), b->getButtonText()) + 18;
    b->setBounds(header.removeFromLeft(w).reduced(2, 3));
  }
  r.removeFromTop(8);
  auto bottom = r.removeFromBottom(22);
  status_.setBounds(bottom.removeFromLeft(bottom.getWidth() / 2));
  cost_.setBounds(bottom);
  r.removeFromBottom(6);
  if (apply_.isVisible() || discard_.isVisible() || like_.isVisible()) {
    auto buttons = r.removeFromBottom(30);
    const int w = std::min(110, (buttons.getWidth() - 18) / 4);
    apply_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    discard_.setBounds(buttons.removeFromLeft(w));
    dislike_.setBounds(buttons.removeFromRight(w));
    buttons.removeFromRight(6);
    like_.setBounds(buttons.removeFromRight(w));
    r.removeFromBottom(6);
  }
  auto entry = r.removeFromBottom(64);
  send_.setBounds(entry.removeFromRight(70).withSizeKeepingCentre(70, 30));
  entry.removeFromRight(6);
  input_.setBounds(entry);
  r.removeFromBottom(8);
  transcript_.setBounds(r);
}

}  // namespace core::app
