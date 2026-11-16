#include "AiPanel.h"
#include "synth.hpp"
#include "models.hpp"
#include "samples.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <thread>
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

int64_t nowMs() { return juce::Time::currentTimeMillis(); }

std::filesystem::path pathOfFile(const juce::File& f) {
  const juce::String p = f.getFullPathName();
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.toRawUTF8())));
}

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
    for (int i = 0; i < (int)owner_.models_.size(); ++i) model_.addItem(u8(owner_.models_[(size_t)i].name), i + 1);
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
      const auto at = std::find(modes.begin(), modes.end(), owner_.configNow().permissions.mode);
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
    label(placeLabel_, "PLACE");
    place_.addItem(u8("At the right side"), 1);
    place_.addItem(u8("At the bottom"), 2);
    place_.setSelectedId(owner_.prefs_.dock == Dock::Side ? 1 : 2, juce::dontSendNotification);
    place_.onChange = [this] { owner_.setDock(place_.getSelectedId() == 1 ? Dock::Side : Dock::Bottom); };
    addAndMakeVisible(place_);
    const int n = owner_.memory_ ? owner_.memory_->size() : 0;
    count_.setText(juce::String(n) + (n == 1 ? u8(" recipe in the memory") : u8(" recipes in the memory")), juce::dontSendNotification);
    count_.setColour(juce::Label::textColourId, kMuted);
    addAndMakeVisible(count_);
    // The plugins the AI uses (Diogo, 2026-11-16): look for those only, not the whole VST3 folder.
    label(pluginsLabel_, "PLUGINS THE AI USES");
    pluginStatus_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(pluginStatus_);
    scanAi_.setTooltip(u8("Scans only the AI's plugins (quick); the rest of the plugin list stays as it is"));
    scanAi_.onClick = [this] { scanAiPlugins(); };
    addAndMakeVisible(scanAi_);
    getPlugin_.onClick = [this] {
      for (const auto& p : ai::aiPlugins())
        if (!ai::findAiPlugin(p)) juce::URL(u8(p.download)).launchInDefaultBrowser();
    };
    addChildComponent(getPlugin_);
    refreshPlugins();
    setSize(420, 520);
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
    row(pluginsLabel_, 18);
    row(pluginStatus_, 22);
    auto pb = r.removeFromTop(kSpaceButton);
    scanAi_.setBounds(pb.removeFromLeft(190));
    pb.removeFromLeft(6);
    getPlugin_.setBounds(pb.removeFromLeft(140));
  }

private:
  void changed() { owner_.savePrefs(); }

  // Each plugin of the AI: on this computer or not, and in Core's scanned list or not.
  void refreshPlugins() {
    juce::StringArray lines;
    bool missing = false;
    const auto known = owner_.host_.instruments ? owner_.host_.instruments() : std::vector<json>();
    for (const auto& p : ai::aiPlugins()) {
      const auto file = ai::findAiPlugin(p);
      bool listed = false;
      for (const auto& k : known)
        listed |= juce::String::fromUTF8(k.value("name", std::string()).c_str()).equalsIgnoreCase(u8(p.name)) ||
                  juce::String::fromUTF8(k.value("file", std::string()).c_str()).endsWithIgnoreCase(u8(p.file));
      missing |= !file;
      lines.add(u8(p.name) + u8(": ") +
                (!file ? u8("not installed (free at ") + u8(p.download) + ")"
                       : listed ? u8("installed and in Core's list") : u8("installed, not scanned yet")));
    }
    pluginStatus_.setText(lines.joinIntoString("  ·  "), juce::dontSendNotification);
    getPlugin_.setButtonText(u8("Get ") + u8(ai::synth().name));
    getPlugin_.setVisible(missing);
  }

  void scanAiPlugins() {
    juce::Array<juce::File> files;
    for (const auto& p : ai::aiPlugins())
      if (const auto f = ai::findAiPlugin(p)) files.add(juce::File(juce::String(f->wstring().c_str())));
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
  juce::Label pluginsLabel_, pluginStatus_;
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
  settings_.setTooltip(u8("Model, effort, what the AI may do, the memory, where this panel goes"));
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
    std::string why;
    memory_ = ai::Memory::open(pathOfFile(host_.keyFolder.getChildFile("core-ai-memory.sqlite")), &why);
    if (!memory_) say(u8("Core"), u8("The AI's memory could not be opened: ") + u8(why));
  }
  {
    const juce::File settings = host_.keyFolder.getChildFile("ai-settings.json");
    const json j = settings.existsAsFile() ? json::parse(settings.loadFileAsString().toStdString(), nullptr, false) : json();
    std::vector<std::string> problems;
    models_ = ai::modelEntries(j.is_discarded() ? json() : j, &problems);
    permissions_ = ai::permissionsFrom(j.is_discarded() ? json() : j, &problems);
    for (const auto& p : problems) say(u8("Core"), u8("ai-settings.json: ") + u8(p));
  }
  loadPrefs();
  status_.setColour(juce::Label::textColourId, kText.withAlpha(0.7f));
  addAndMakeVisible(status_);
  cost_.setJustificationType(juce::Justification::centredRight);
  cost_.setColour(juce::Label::textColourId, kText.withAlpha(0.7f));
  addAndMakeVisible(cost_);
  if (apiKey().isEmpty() && needsKey())
    say(u8("Core"), u8("No key for ") + u8(entry().name) + u8(" yet. Set ANTHROPIC_API_KEY, or put the key alone in ") +
                        host_.keyFolder.getChildFile("anthropic-key.txt").getFullPathName() + u8("."));
  if (host_.knowledge == nullptr) say(u8("Core"), u8("The style knowledge base (ai/core-ai-knowledge.sqlite) is not installed: the AI works without it."));
  said_.clear();  // the notices above are not part of a chat
  grip_->update();
  setSize(prefs_.width, 600);
}

AiPanel::~AiPanel() {
  alive_->store(false);
  saveChat();
}

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
  for (int i = 0; i < (int)models_.size(); ++i)
    if (u8(models_[(size_t)i].name) == prefs_.model) return i;
  return 0;
}

const ai::ModelEntry& AiPanel::entry() const { return models_[(size_t)modelIndex()]; }

juce::String AiPanel::apiKey() const {
  const ai::ModelEntry& e = entry();
  if (!e.keyEnv.empty())
    if (const char* env = std::getenv(e.keyEnv.c_str()); env && *env) return juce::String(env).trim();
  if (e.keyFile.empty()) return {};
  const juce::File f = host_.keyFolder.getChildFile(u8(e.keyFile));
  return f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
}

bool AiPanel::needsKey() const { return !entry().keyEnv.empty() || !entry().keyFile.empty(); }

ai::Config AiPanel::configNow() const {
  ai::Config c = entry().config;
  c.provider.apiKey = apiKey().toStdString();
  c.effort = kEfforts[juce::jlimit(0, (int)std::size(kEfforts) - 1, prefs_.effort)];
  c.maxUsdPerMessage = prefs_.maxUsd;
  c.stream = true;  // the answer shows as it comes (Anthropic; other providers are not streamed)
  c.permissions = permissions_;
  for (const auto& m : ai::permissionModes())
    if (u8(m) == prefs_.mode) c.permissions.mode = m;
  return c;
}

void AiPanel::say(const juce::String& who, const juce::String& text) {
  said_.push_back({who, text});
  transcript_.moveCaretToEnd();
  transcript_.insertTextAtCaret((transcript_.isEmpty() ? juce::String() : juce::String("\n\n")) + who + ": " + text);
}

void AiPanel::send() {
  const juce::String text = input_.getText().trim();
  if (busy_ || text.isEmpty()) return;
  if (needsKey() && apiKey().isEmpty()) {
    say(u8("Core"), u8("No key for ") + u8(entry().name) + u8(": set ") + u8(entry().keyEnv.empty() ? std::string("the key") : entry().keyEnv) +
                        (entry().keyFile.empty() ? juce::String() : u8(" or write it into ") + host_.keyFolder.getChildFile(u8(entry().keyFile)).getFullPathName()));
    return;
  }
  const ai::Config cfg = configNow();
  dropProposal();  // a new message: the old proposal goes
  ai::Memory* shared = prefs_.useMemory ? memory_.get() : nullptr;
  if (!chat_ || chat_->config().model != cfg.model || chat_->config().effort != cfg.effort || chat_->config().provider.url() != cfg.provider.url() ||
      chat_->config().permissions.mode != cfg.permissions.mode || chat_->config().provider.apiKey != cfg.provider.apiKey || chatMemory_ != shared) {
    // Another model, effort, mode or memory setting: the same conversation goes on with it.
    const json kept = chat_ ? chat_->messages() : json::array();
    chat_ = std::make_unique<ai::Chat>(cfg, host_.functions(), host_.pcore, host_.knowledge, shared);
    chat_->restore(kept);
    chatMemory_ = shared;
  }
  if (chatTitle_.isEmpty()) chatTitle_ = text.substring(0, 60);
  input_.clear();
  showFeedback(false);
  lastRequest_ = text.toStdString();
  // The selection on the tracks: the AI changes only those tracks, in those bars.
  const control::Zone zone = host_.zone ? host_.zone() : control::Zone{};
  juce::String shown = text;
  if (zone.active()) {
    const int64_t bar = 4 * doc::kPpq;
    shown += u8("\n(selection: ") + juce::String((int)zone.tracks.size()) + (zone.tracks.size() == 1 ? u8(" track, bars ") : u8(" tracks, bars ")) +
             juce::String((int)(zone.startTick / bar + 1)) + "-" + juce::String((int)(zone.endTick / bar)) + ")";
  }
  say(u8("Diogo"), shown);
  busy_ = true;
  send_.setEnabled(false);
  status_.setText(u8("Thinking…"), juce::dontSendNotification);
  post(chat_->ask(text.toStdString(), host_.song(), zone.active() ? &zone : nullptr));
}

void AiPanel::post(json body) {
  const ai::HttpRequest req = chat_->http(body);
  const bool streaming = req.body.value("stream", false);
  const std::string data = req.body.dump(-1, ' ', false, json::error_handler_t::replace);
  juce::String headers;
  for (const auto& [k, v] : req.headers) headers += u8(k) + ": " + u8(v) + "\r\n";
  const juce::String endpoint = u8(req.url);
  live_.clear();
  std::thread([this, alive = alive_, headers, endpoint, data, streaming] {
    json response;
    juce::String failure;
    int status = 0;
    const juce::URL url = juce::URL(endpoint).withPOSTData(juce::String::fromUTF8(data.c_str()));
    const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                             .withExtraHeaders(headers.trimEnd())
                             .withConnectionTimeoutMs(180000)
                             .withStatusCode(&status);
    if (auto stream = url.createInputStream(options)) {
      if (streaming) {
        // The answer as it comes (A8): the text and the calls show in the status line while the model works; the
        // finished message is the same one a request without streaming gets.
        ai::StreamAssembler a;
        a.onText = [this, alive](const std::string& t) {
          juce::MessageManager::callAsync([this, alive, t] {
            if (alive->load()) liveText(u8(t));
          });
        };
        a.onTool = [this, alive](const std::string& name) {
          juce::MessageManager::callAsync([this, alive, name] {
            if (alive->load()) liveText(u8("\n[") + u8(name) + u8("] "));
          });
        };
        std::string raw;
        char buf[4096];
        for (int n; (n = stream->read(buf, (int)sizeof buf)) > 0;) {
          a.feed(std::string_view(buf, (size_t)n));
          if (!a.sawEvents() && raw.size() < (1u << 20)) raw.append(buf, (size_t)n);
        }
        a.finish();
        if (a.sawEvents()) {
          response = a.message();
        } else {  // not a stream: an HTTP error's JSON
          response = json::parse(raw, nullptr, false);
          if (response.is_discarded()) failure = u8("the answer is not JSON (HTTP ") + juce::String(status) + ")";
        }
      } else {
        const juce::String text = stream->readEntireStreamAsString();
        response = json::parse(text.toStdString(), nullptr, false);
        if (response.is_discarded()) failure = u8("the answer is not JSON (HTTP ") + juce::String(status) + ")";
      }
    } else {
      failure = u8("no connection to ") + endpoint;
    }
    juce::MessageManager::callAsync([this, alive, response, failure] {
      if (alive->load()) answered(response, failure);
    });
  }).detach();
}

void AiPanel::liveText(const juce::String& piece) {
  if (!busy_) return;
  live_ += piece;
  // The status line shows the end of what has come so far (one line: the panel stays as it is).
  juce::String shown = live_.replace("\n", " ").trim();
  if (shown.length() > 160) shown = juce::String::fromUTF8("\xe2\x80\xa6") + shown.getLastCharacters(160);
  status_.setText(shown, juce::dontSendNotification);
}

void AiPanel::answered(const json& response, const juce::String& failure) {
  if (!chat_) return;
  if (failure.isNotEmpty()) {
    chat_->abandon();  // the message never got an answer: it leaves the conversation
    say(u8("Core"), failure + u8(" (the message was not kept: send it again)"));
    finished();
    return;
  }
  const json next = chat_->onResponse(response, host_.song());
  showCost();
  if (!next.is_null()) {
    if (live_.isEmpty())
      status_.setText(u8("Working… ") + juce::String((int)chat_->log().size()) + u8(" call(s) so far"), juce::dontSendNotification);
    post(next);
    return;
  }
  finished();
}

void AiPanel::finished() {
  busy_ = false;
  send_.setEnabled(true);
  status_.setText({}, juce::dontSendNotification);
  if (!chat_) return;
  if (!chat_->reply().empty()) say(u8("AI"), u8(chat_->reply()));
  for (const auto& line : chat_->log()) say(u8("Core"), u8(line));
  if (!chat_->error().empty()) say(u8("Core"), u8(chat_->error()));
  const json proposal = chat_->proposal();
  if (!proposal["calls"].empty()) {
    pending_ = proposal;
    showProposal();
    if (proposal.value("auto", false) && apply_.isVisible()) applyProposal();  // trusted: every call allowed
  }
  showCost();
  saveChat();  // after every answer: closing Core or New never loses it
}

// The proposal as Core checks it against the song now: what it calls, warnings, what is left out.
void AiPanel::showProposal() {
  const control::Proposal p = control::validateProposal(pending_, host_.functions(), host_.song(), nullptr);
  juce::String text = u8("Proposal") + (p.summary.empty() ? juce::String() : ": " + u8(p.summary));
  for (const auto& c : p.calls) text += "\n  \u2022 " + u8(control::describeCall(c));
  for (const auto& w : p.warnings()) text += "\n  ! " + u8(w);
  for (const auto& x : p.problems) text += "\n  \u2717 " + u8(x);
  say(u8("Core"), text);
  apply_.setVisible(!p.calls.empty());
  discard_.setVisible(true);
  resized();
}

void AiPanel::applyProposal() {
  if (pending_.is_null() || !host_.apply) return;
  try {
    const auto lines = host_.apply(pending_);
    juce::String text = u8("Applied (Edit > Undo takes it back):");
    for (const auto& l : lines) text += "\n  \u2022 " + u8(l);
    say(u8("Core"), text);
    if (memory_ && chat_ && prefs_.keepMemory) {  // a candidate recipe; Gostei confirms it
      recipeId_ = memory_->applied(lastRequest_, doc::songStyle(host_.song()), pending_, chat_->config().model, chat_->shown(), nowMs(),
                                   chat_->episode());
      showFeedback(!recipeId_.empty());
    }
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
  if (!memory_ || recipeId_.empty()) return;
  if (memory_->feedback(recipeId_, liked, "", nowMs()))
    say(u8("Core"), liked ? u8("Kept in the AI's memory: similar requests start from it.") : u8("Noted: the AI will not offer this again."));
  showFeedback(false);
}

void AiPanel::memoryAction(int choice) {
  if (!memory_ || choice < 1 || choice > 3) return;
  const bool exporting = choice != 2, lines = choice == 3;
  const char* name = lines ? "core-ai-memory.jsonl" : "core-ai-memory.json";
  chooser_ = std::make_unique<juce::FileChooser>(exporting ? u8("Export the AI's memory") : u8("Import an AI memory"),
                                                 juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(name),
                                                 lines ? "*.jsonl" : "*.json");
  const int flags = exporting ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode;
  chooser_->launchAsync(flags | juce::FileBrowserComponent::canSelectFiles, [this, alive = alive_, exporting, lines](const juce::FileChooser& fc) {
    if (!alive->load() || !memory_ || fc.getResult() == juce::File()) return;
    const juce::File f = fc.getResult();
    if (exporting) {
      const std::string text = lines ? memory_->exportJsonl() : memory_->exportJson().dump(1, ' ', false, json::error_handler_t::replace);
      const bool ok = f.replaceWithText(u8(text));
      say(u8("Core"), ok ? u8("Memory exported to ") + f.getFullPathName() : u8("Could not write ") + f.getFullPathName());
      return;
    }
    const json j = json::parse(f.loadFileAsString().toStdString(), nullptr, false);
    std::string why;
    const int n = j.is_discarded() ? -1 : memory_->importJson(j, &why);
    say(u8("Core"), n < 0 ? u8("Not imported: ") + u8(why.empty() ? std::string("not JSON") : why) : u8("Imported ") + juce::String(n) + u8(" new recipe(s)."));
  });
}

// ---- chats: kept in %APPDATA%\Core\AI\chats, one file each, after every answer

juce::File AiPanel::chatsFolder() const { return host_.keyFolder.getChildFile("chats"); }

void AiPanel::saveChat() {
  if (said_.empty() || !chat_ || chat_->messages().empty()) return;
  if (chatFile_ == juce::File()) {
    chatsFolder().createDirectory();
    chatFile_ = chatsFolder().getNonexistentChildFile("chat-" + juce::Time::getCurrentTime().formatted("%Y-%m-%d-%H%M%S"), ".json", false);
  }
  json transcript = json::array();
  for (const auto& [who, text] : said_) transcript.push_back({{"who", who.toStdString()}, {"text", text.toStdString()}});
  const json j = {{"format", "core-ai-chat"}, {"version", 1}, {"title", chatTitle_.toStdString()}, {"updatedMs", nowMs()},
                  {"model", chat_->config().model}, {"costUsd", chat_->cost(chat_->total())}, {"transcript", transcript},
                  {"messages", chat_->messages()}};
  chatFile_.replaceWithText(u8(j.dump(-1, ' ', false, json::error_handler_t::replace)));
}

void AiPanel::newChat() {
  if (busy_) return;
  saveChat();
  chat_.reset();
  chatMemory_ = nullptr;
  chatFile_ = juce::File();
  chatTitle_.clear();
  said_.clear();
  transcript_.clear();
  dropProposal();
  showFeedback(false);
  status_.setText({}, juce::dontSendNotification);
  cost_.setText({}, juce::dontSendNotification);
  focusInput();
}

void AiPanel::openChat(const juce::File& file) {
  if (busy_) return;
  const json j = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
  if (!j.is_object() || j.value("format", std::string()) != "core-ai-chat") {
    say(u8("Core"), u8("Not a chat of Core's AI: ") + file.getFullPathName());
    return;
  }
  newChat();
  ai::Memory* shared = prefs_.useMemory ? memory_.get() : nullptr;
  chat_ = std::make_unique<ai::Chat>(configNow(), host_.functions(), host_.pcore, host_.knowledge, shared);
  chat_->restore(j.value("messages", json::array()));
  chatMemory_ = shared;
  chatFile_ = file;
  chatTitle_ = u8(j.value("title", std::string()));
  for (const auto& t : j.value("transcript", json::array()))
    if (t.is_object()) say(u8(t.value("who", std::string())), u8(t.value("text", std::string())));
  focusInput();
}

void AiPanel::deleteChat() {
  if (busy_ || (said_.empty() && chatFile_ == juce::File())) return;
  juce::Component::SafePointer<AiPanel> safe(this);
  juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
                                   .withTitle(u8("Delete this chat"))
                                   .withMessage(u8("The conversation leaves the screen and Chats. The AI's memory keeps its recipes."))
                                   .withButton(u8("Delete"), 1).withButton(u8("Cancel"), 0),
                               [safe](int result) {
                                 if (result != 1 || safe == nullptr) return;
                                 const juce::File f = safe->chatFile_;
                                 safe->said_.clear();  // nothing left to keep
                                 safe->chatFile_ = juce::File();
                                 if (f.existsAsFile()) f.deleteFile();
                                 safe->newChat();
                               });
}

void AiPanel::showChats() {
  juce::Array<juce::File> files = chatsFolder().findChildFiles(juce::File::findFiles, false, "chat-*.json");
  std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
  juce::PopupMenu menu;
  menu.addSectionHeader(u8("EARLIER CHATS"));
  const int shown = std::min(files.size(), 30);
  for (int i = 0; i < shown; ++i) {
    const json j = json::parse(files[i].loadFileAsString().toStdString(), nullptr, false);
    juce::String title = j.is_object() ? u8(j.value("title", std::string())) : juce::String();
    if (title.isEmpty()) title = files[i].getFileNameWithoutExtension();
    menu.addItem(juce::PopupMenu::Item(files[i].getLastModificationTime().formatted("%d/%m %H:%M") + "  " + title)
                     .setID(i + 1).setTicked(files[i] == chatFile_));
  }
  if (shown == 0) menu.addItem(juce::PopupMenu::Item(u8("No chats yet")).setEnabled(false));
  menu.addSeparator();
  menu.addItem(juce::PopupMenu::Item(u8("Open the chats folder")).setID(1000));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&chats_), [this, alive = alive_, files](int id) {
    if (!alive->load() || id == 0) return;
    if (id == 1000) {
      chatsFolder().createDirectory();
      chatsFolder().startAsProcess();
      return;
    }
    if (id - 1 < files.size()) openChat(files[id - 1]);
  });
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
  juce::CallOutBox::launchAsynchronously(std::make_unique<Settings>(*this), settings_.getScreenBounds(), nullptr);
}

void AiPanel::dropProposal() {
  pending_ = nullptr;
  apply_.setVisible(false);
  discard_.setVisible(false);
  resized();
}

void AiPanel::showCost() {
  if (!chat_) return;
  const ai::Usage& u = chat_->usage();
  const ai::Usage& t = chat_->total();
  cost_.setText(u8("This message ") + dollars(chat_->cost(u)) + " (" + juce::String(u.requests) + u8(" requests) · chat ") + dollars(chat_->cost(t)),
                juce::dontSendNotification);
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
