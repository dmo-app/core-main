#include "AiPanel.h"
#include "models.hpp"
#include <algorithm>
#include <cstdlib>
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
// The permission modes as the combo shows them (catalog.hpp: ask, readonly, trusted).
constexpr const char* kModes[] = {"Ask before changing", "Read only", "Trusted (applies by itself)"};

juce::String dollars(double usd) { return "$" + juce::String(usd, usd < 0.1 ? 4 : 2); }

}  // namespace

AiPanel::AiPanel(Host host) : host_(std::move(host)) {
  transcript_.setMultiLine(true, true);
  transcript_.setReadOnly(true);
  transcript_.setScrollbarsShown(true);
  transcript_.setCaretVisible(false);
  addAndMakeVisible(transcript_);
  input_.setMultiLine(true, true);
  input_.setReturnKeyStartsNewLine(false);
  input_.setTextToShowWhenEmpty(u8("Ask for a beat, a song structure or a change to a part…"), kText.withAlpha(0.4f));
  input_.onReturnKey = [this] { send(); };
  addAndMakeVisible(input_);
  send_.onClick = [this] { send(); };
  addAndMakeVisible(send_);
  newChat_.onClick = [this] {
    if (busy_) return;
    if (chat_) chat_->reset();
    transcript_.clear();
    status_.setText({}, juce::dontSendNotification);
  };
  addAndMakeVisible(newChat_);
  apply_.onClick = [this] { applyProposal(); };
  apply_.setTooltip(u8("Makes the proposal on the song as one step of Undo"));
  discard_.onClick = [this] { dropProposal(); };
  addChildComponent(apply_);
  addChildComponent(discard_);
  {
    const juce::File settings = host_.keyFolder.getChildFile("ai-settings.json");
    const json j = settings.existsAsFile() ? json::parse(settings.loadFileAsString().toStdString(), nullptr, false) : json();
    std::vector<std::string> problems;
    models_ = ai::modelEntries(j.is_discarded() ? json() : j, &problems);
    permissions_ = ai::permissionsFrom(j.is_discarded() ? json() : j, &problems);
    for (const auto& p : problems) say(u8("Core"), u8("ai-settings.json: ") + u8(p));
  }
  for (int i = 0; i < (int)models_.size(); ++i) model_.addItem(u8(models_[(size_t)i].name), i + 1);
  model_.setSelectedId(1, juce::dontSendNotification);
  model_.setTooltip(u8("The model: Claude Sonnet for the music, Haiku for small changes at a fraction of the cost; more (other APIs, local models) in ai-settings.json"));
  addAndMakeVisible(model_);
  for (int i = 0; i < (int)std::size(kEfforts); ++i) effort_.addItem(u8(kEfforts[i]), i + 1);
  effort_.setSelectedId(2, juce::dontSendNotification);
  effort_.setTooltip(u8("How much the model thinks: low is quicker and cheaper"));
  addAndMakeVisible(effort_);
  for (int i = 0; i < (int)std::size(kModes); ++i) mode_.addItem(u8(kModes[i]), i + 1);
  {
    const auto& modes = ai::permissionModes();
    const auto at = std::find(modes.begin(), modes.end(), permissions_.mode);
    mode_.setSelectedId(at == modes.end() ? 1 : (int)(at - modes.begin()) + 1, juce::dontSendNotification);
  }
  mode_.setTooltip(u8("What the AI may do: ask before changing the song (Apply), only read and explain, or apply by itself (still one Undo step). "
                      "Per action in ai-settings.json: \"permissions\": {\"allow\": [...], \"deny\": [...], \"ask\": [...]}"));
  addAndMakeVisible(mode_);
  status_.setColour(juce::Label::textColourId, kText.withAlpha(0.7f));
  addAndMakeVisible(status_);
  cost_.setJustificationType(juce::Justification::centredRight);
  cost_.setColour(juce::Label::textColourId, kText.withAlpha(0.7f));
  addAndMakeVisible(cost_);
  if (apiKey().isEmpty())
    say(u8("Core"), u8("No Anthropic key yet. Set ANTHROPIC_API_KEY, or put the key alone in ") +
                        host_.keyFolder.getChildFile("anthropic-key.txt").getFullPathName() + u8("."));
  if (host_.knowledge == nullptr) say(u8("Core"), u8("The style knowledge base (ai/core-ai-knowledge.sqlite) is not installed: the AI works without it."));
  setSize(600, 640);
}

AiPanel::~AiPanel() { alive_->store(false); }

const ai::ModelEntry& AiPanel::entry() const {
  return models_[(size_t)juce::jlimit(0, (int)models_.size() - 1, model_.getSelectedId() - 1)];
}

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
  c.effort = kEfforts[juce::jlimit(0, (int)std::size(kEfforts) - 1, effort_.getSelectedId() - 1)];
  c.permissions = permissions_;
  c.permissions.mode = ai::permissionModes()[(size_t)juce::jlimit(0, (int)ai::permissionModes().size() - 1, mode_.getSelectedId() - 1)];
  return c;
}

void AiPanel::say(const juce::String& who, const juce::String& text) {
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
  if (!chat_ || chat_->config().model != cfg.model || chat_->config().effort != cfg.effort || chat_->config().provider.url() != cfg.provider.url() ||
      chat_->config().permissions.mode != cfg.permissions.mode)
    chat_ = std::make_unique<ai::Chat>(cfg, host_.functions(), host_.pcore, host_.knowledge);  // a new model or mode starts a new chat
  input_.clear();
  say(u8("Diogo"), text);
  busy_ = true;
  send_.setEnabled(false);
  status_.setText(u8("Thinking…"), juce::dontSendNotification);
  post(chat_->ask(text.toStdString(), host_.song()));
}

void AiPanel::post(json body) {
  const ai::HttpRequest req = chat_->http(body);
  const std::string data = req.body.dump(-1, ' ', false, json::error_handler_t::replace);
  juce::String headers;
  for (const auto& [k, v] : req.headers) headers += u8(k) + ": " + u8(v) + "\r\n";
  const juce::String endpoint = u8(req.url);
  std::thread([this, alive = alive_, headers, endpoint, data] {
    json response;
    juce::String failure;
    int status = 0;
    const juce::URL url = juce::URL(endpoint).withPOSTData(juce::String::fromUTF8(data.c_str()));
    const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                             .withExtraHeaders(headers.trimEnd())
                             .withConnectionTimeoutMs(180000)
                             .withStatusCode(&status);
    if (auto stream = url.createInputStream(options)) {
      const juce::String text = stream->readEntireStreamAsString();
      response = json::parse(text.toStdString(), nullptr, false);
      if (response.is_discarded()) failure = u8("the answer is not JSON (HTTP ") + juce::String(status) + ")";
    } else {
      failure = u8("no connection to ") + endpoint;
    }
    juce::MessageManager::callAsync([this, alive, response, failure] {
      if (alive->load()) answered(response, failure);
    });
  }).detach();
}

void AiPanel::answered(const json& response, const juce::String& failure) {
  if (!chat_) return;
  if (failure.isNotEmpty()) {
    say(u8("Core"), failure);
    finished();
    return;
  }
  const json next = chat_->onResponse(response, host_.song());
  showCost();
  if (!next.is_null()) {
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
  } catch (const std::exception& e) {
    say(u8("Core"), u8("Not applied, nothing changed: ") + u8(e.what()));
    return;
  }
  dropProposal();
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

void AiPanel::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void AiPanel::resized() {
  auto r = getLocalBounds().reduced(10);
  auto top = r.removeFromTop(26);
  model_.setBounds(top.removeFromLeft(180));
  top.removeFromLeft(6);
  effort_.setBounds(top.removeFromLeft(80));
  top.removeFromLeft(6);
  mode_.setBounds(top.removeFromLeft(170));
  newChat_.setBounds(top.removeFromRight(90));
  r.removeFromTop(8);
  auto bottom = r.removeFromBottom(22);
  status_.setBounds(bottom.removeFromLeft(bottom.getWidth() / 2));
  cost_.setBounds(bottom);
  r.removeFromBottom(6);
  if (apply_.isVisible() || discard_.isVisible()) {
    auto buttons = r.removeFromBottom(30);
    apply_.setBounds(buttons.removeFromLeft(110));
    buttons.removeFromLeft(6);
    discard_.setBounds(buttons.removeFromLeft(110));
    r.removeFromBottom(6);
  }
  auto entry = r.removeFromBottom(64);
  send_.setBounds(entry.removeFromRight(80).withSizeKeepingCentre(80, 30));
  entry.removeFromRight(6);
  input_.setBounds(entry);
  r.removeFromBottom(8);
  transcript_.setBounds(r);
}

AiWindow::AiWindow(AiPanel::Host host) : juce::DocumentWindow(u8("Core · AI"), kBackground, juce::DocumentWindow::allButtons) {
  setUsingNativeTitleBar(true);
  setContentOwned(new AiPanel(std::move(host)), true);
  setResizable(true, true);
  setResizeLimits(560, 420, 2000, 2000);
  centreWithSize(600, 640);
}

void AiWindow::show() {
  setVisible(true);
  toFront(true);
}

}  // namespace core::app
