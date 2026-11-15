// File > Make training clips... (Diogo, 2026-11-05): short excerpts of the style templates (20-30 s, whole bars), each a
// WAV and a JSON with everything Core knows about it (style, BPM, the sample of each track and its catalogue
// entry, the effects with their settings, the rhythm, the seed), for AICORE to turn into a training set for ACE.
// The work is done by core-clips (beside Core.exe, app/domain/tools/core_clips.cpp), which renders each clip with
// core-engine below normal priority; this window chooses what to make, shows its lines and stops it. The clips go
// to Settings > Folders > Training clips, one folder per batch. Nothing here changes the song.
#include "MainComponent.h"
#include "Theme.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace core::app {
using namespace theme;

juce::File MainComponent::trainingClipsFolder() const {
  const juce::File chosen = folderSetting("folderTrainingClips");
  if (chosen != juce::File()) return chosen;
  return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Core training clips");
}

class MainComponent::TrainingClipsPanel final : public juce::Component, private juce::Timer {
 public:
  explicit TrainingClipsPanel(MainComponent& owner) : o_(owner) {
    auto title = [this](juce::Label& l, const juce::String& text) {
      l.setText(text, juce::dontSendNotification);
      l.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
      l.setColour(juce::Label::textColourId, kText);
      addAndMakeVisible(l);
    };
    title(styleLabel_, "Style");
    title(kindsLabel_, "Clips of");
    title(countLabel_, "Per template");
    title(seedLabel_, "First seed");
    title(folderLabel_, "Folder");

    style_.addItem("All styles", 1);
    std::set<std::string> styles;
    for (const auto& t : o_.templates_.templates) if (!t.tmpl.info.style.empty()) styles.insert(t.tmpl.info.style);
    int id = 2;
    for (const auto& s : styles) style_.addItem(juce::String::fromUTF8(s.c_str()), id++);
    style_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(style_);

    kbbb_.setButtonText("Kick & bass (KBBB)");
    groove_.setButtonText("Drums & bass");
    full_.setButtonText("Full mix");
    kbbb_.setToggleState(true, juce::dontSendNotification);
    groove_.setToggleState(true, juce::dontSendNotification);
    for (auto* b : {&kbbb_, &groove_, &full_, &samples_}) addAndMakeVisible(b);
    samples_.setButtonText("Vary the samples: other ones from my library that fit (off: the template's own)");
    samples_.setToggleState(false, juce::dontSendNotification);
    samples_.setEnabled(o_.sampleIndexFile().existsAsFile());

    for (auto* e : {&count_, &seed_}) {
      e->setInputRestrictions(6, "0123456789");
      e->setJustification(juce::Justification::centredLeft);
      addAndMakeVisible(e);
    }
    count_.setText("2", false);
    seed_.setText("1", false);
    count_.setTooltip("How many clips of each kind for each template (seeds go up by one)");
    seed_.setTooltip("The same seed gives the same clip again; a new seed, a new one");

    folder_.setColour(juce::Label::textColourId, kMuted);
    addAndMakeVisible(folder_);
    settings_.setButtonText(juce::String::fromUTF8("Change\xe2\x80\xa6"));
    settings_.onClick = [this] { o_.openSettings(); };
    addAndMakeVisible(settings_);

    start_.onClick = [this] { running() ? stop() : start(); };
    open_.setButtonText("Open folder");
    open_.onClick = [this] {
      const juce::File f = lastBatch_ != juce::File() ? lastBatch_ : o_.trainingClipsFolder();
      if (f.isDirectory()) f.startAsProcess();
    };
    for (auto* b : {&start_, &open_}) addAndMakeVisible(b);

    log_.setMultiLine(true);
    log_.setReadOnly(true);
    log_.setScrollbarsShown(true);
    log_.setFont(juce::FontOptions(theme::kFontBody));
    addAndMakeVisible(log_);
    log_.setText("Each clip is a WAV and a JSON with its facts (style, BPM, samples, effects and their settings, rhythm, seed).\n"
                 "Clips with silence or clipping are left out and listed in manifest.jsonl.\n"
                 "It renders below normal priority; making clips while you listen may still cost the engine some headroom.\n");
    refresh();
    startTimer(150);
    setSize(640, 520);
  }

  ~TrainingClipsPanel() override {
    stopTimer();
    stop();
    if (worker_.joinable()) worker_.join();
  }

  void paint(juce::Graphics& g) override { g.fillAll(kPanel); }

  void resized() override {
    auto r = getLocalBounds().reduced(16, 12);
    auto row = [&r](int h) { auto x = r.removeFromTop(h); r.removeFromTop(8); return x; };
    auto line = row(28);
    styleLabel_.setBounds(line.removeFromLeft(120));
    style_.setBounds(line.removeFromLeft(240));
    line = row(26);
    kindsLabel_.setBounds(line.removeFromLeft(120));
    kbbb_.setBounds(line.removeFromLeft(170));
    groove_.setBounds(line.removeFromLeft(140));
    full_.setBounds(line.removeFromLeft(110));
    line = row(28);
    countLabel_.setBounds(line.removeFromLeft(120));
    count_.setBounds(line.removeFromLeft(70));
    line.removeFromLeft(30);
    seedLabel_.setBounds(line.removeFromLeft(100));
    seed_.setBounds(line.removeFromLeft(90));
    line = row(26);
    line.removeFromLeft(120);
    samples_.setBounds(line);
    line = row(28);
    folderLabel_.setBounds(line.removeFromLeft(120));
    settings_.setBounds(line.removeFromRight(90));
    folder_.setBounds(line);
    line = row(30);
    line.removeFromLeft(120);
    start_.setBounds(line.removeFromLeft(130));
    line.removeFromLeft(8);
    open_.setBounds(line.removeFromLeft(110));
    log_.setBounds(r);
  }

 private:
  bool running() const { return worker_.joinable() && !ended_.load(); }

  juce::File toolFile() const {
    const juce::File dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
#if JUCE_WINDOWS
    return dir.getChildFile("core-clips.exe");
#else
    return dir.getChildFile("core-clips");
#endif
  }

  void start() {
    if (worker_.joinable()) worker_.join();
    juce::StringArray kinds;
    if (kbbb_.getToggleState()) kinds.add("kbbb");
    if (groove_.getToggleState()) kinds.add("groove");
    if (full_.getToggleState()) kinds.add("full");
    if (kinds.isEmpty()) { append("Choose at least one kind of clip."); return; }
    const int count = juce::jlimit(1, 500, count_.getText().getIntValue());
    const juce::File tool = toolFile();
    if (!tool.existsAsFile()) { append("core-clips is missing beside Core.exe (" + tool.getFullPathName() + ")."); return; }
    const juce::File out = o_.trainingClipsFolder();
    if (!out.isDirectory() && !out.createDirectory()) { append("Could not make the folder " + out.getFullPathName() + "."); return; }
    const juce::String batch = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    lastBatch_ = out.getChildFile(batch);

    juce::StringArray args{tool.getFullPathName(), "--out", out.getFullPathName(), "--batch", batch,
                           "--templates", o_.builtinTemplatesFolder().getFullPathName(), "--kinds", kinds.joinIntoString(","),
                           "--count", juce::String(count), "--seed", juce::String(juce::jmax(0, seed_.getText().getIntValue()))};
    if (o_.userTemplatesFolder().isDirectory()) args.addArray({"--templates", o_.userTemplatesFolder().getFullPathName()});
    if (style_.getSelectedId() > 1) args.addArray({"--style", style_.getText()});
    // The catalogue always (what the facts say about each sample), the folder read again first (files move).
    if (o_.sampleIndexFile().existsAsFile()) args.addArray({"--index", o_.sampleIndexFile().getFullPathName()});
    if (o_.sampleLibraryFolder().isDirectory()) args.addArray({"--library", o_.sampleLibraryFolder().getFullPathName()});
    if (samples_.getToggleState()) args.add("--swap-samples");
    append("\nMaking clips in " + lastBatch_.getFullPathName());
    ended_ = false;
    cancelled_ = false;
    worker_ = std::thread([this, args] { run(args); });
    refresh();
  }

  void stop() {
    cancelled_ = true;
    std::lock_guard<std::mutex> g(mutex_);
    if (process_.isRunning()) process_.kill();
  }

  void run(juce::StringArray args) {
    {
      std::lock_guard<std::mutex> g(mutex_);
      if (cancelled_ || !process_.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
        if (!cancelled_) lines_.push_back("Could not start core-clips.");
        ended_ = true;
        return;
      }
    }
    std::string pending;
    for (char ch = 0; process_.readProcessOutput(&ch, 1) == 1;) {  // a byte at a time: see Stems.cpp
      if (ch != '\n') { pending += ch; continue; }
      if (!pending.empty() && pending.back() == '\r') pending.pop_back();
      std::lock_guard<std::mutex> g(mutex_);
      lines_.push_back(std::move(pending));
      pending.clear();
    }
    process_.waitForProcessToFinish(5000);
    std::lock_guard<std::mutex> g(mutex_);
    if (!pending.empty()) lines_.push_back(pending);
    lines_.push_back(cancelled_ ? "Stopped (the clips made so far stay; the one being rendered may finish on its own)." : "Done.");
    ended_ = true;
  }

  void append(const juce::String& text) {
    log_.moveCaretToEnd();
    log_.insertTextAtCaret(text + "\n");
  }

  void timerCallback() override {
    std::deque<std::string> lines;
    {
      std::lock_guard<std::mutex> g(mutex_);
      lines.swap(lines_);
    }
    for (const auto& l : lines) append(juce::String::fromUTF8(l.c_str()));
    if (running() != wasRunning_) refresh();
  }

  void refresh() {
    wasRunning_ = running();
    start_.setButtonText(wasRunning_ ? "Stop" : "Make clips");
    for (auto* c : std::initializer_list<juce::Component*>{&style_, &kbbb_, &groove_, &full_, &count_, &seed_, &settings_}) c->setEnabled(!wasRunning_);
    samples_.setEnabled(!wasRunning_ && o_.sampleIndexFile().existsAsFile());
    folder_.setText(o_.trainingClipsFolder().getFullPathName(), juce::dontSendNotification);
  }

  MainComponent& o_;
  juce::Label styleLabel_, kindsLabel_, countLabel_, seedLabel_, folderLabel_, folder_;
  juce::ComboBox style_;
  juce::ToggleButton kbbb_, groove_, full_, samples_;
  juce::TextEditor count_, seed_, log_;
  juce::TextButton settings_, start_, open_;
  juce::File lastBatch_;

  std::thread worker_;
  std::mutex mutex_;
  juce::ChildProcess process_;
  std::deque<std::string> lines_;
  std::atomic<bool> ended_{true}, cancelled_{false};
  bool wasRunning_ = false;
};

void MainComponent::openTrainingClips() {
  if (clipsWindow_ != nullptr) {
    clipsWindow_->toFront(true);
    return;
  }
  juce::DialogWindow::LaunchOptions options;
  options.content.setOwned(new TrainingClipsPanel(*this));
  options.dialogTitle = "Make training clips";
  options.dialogBackgroundColour = kPanel;
  options.escapeKeyTriggersCloseButton = true;
  options.useNativeTitleBar = true;
  options.resizable = false;
  options.componentToCentreAround = this;
  clipsWindow_ = options.launchAsync();
}

}  // namespace core::app
