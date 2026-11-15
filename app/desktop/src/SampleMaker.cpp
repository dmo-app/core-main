#include "SampleMaker.h"
#include "Language.h"
#include "Diagnostics.h"
#include "Theme.h"

#include "assets.hpp"
#include "bounce.hpp"
#include "edit.hpp"
#include "samples.hpp"
#include "sampling.hpp"

namespace core::app {
using namespace theme;
namespace {
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }
std::string utf8(const juce::String& s) { return s.toStdString(); }
std::filesystem::path pathOf(const juce::File& f) { return std::filesystem::path(f.getFullPathName().toWideCharPointer()); }
juce::File fileOf(const std::filesystem::path& p) { return juce::File(juce::String(p.wstring().c_str())); }
constexpr int64_t kBar = 4 * doc::kPpq;
constexpr double kMaxSeconds = 10 * 60;  // a sample longer than this is a song: cut the part first

const doc::AudioClip* findClip(const doc::Document& d, const std::string& trackId, const std::string& clipId) {
  if (const doc::Track* t = d.findTrack(trackId))
    for (const auto& c : t->audioClips)
      if (c.id == clipId) return &c;
  return nullptr;
}

// [from, from + frames) of a WAV file, at its own rate. "" or why not.
juce::String readAudio(const juce::File& file, double fromSeconds, double seconds, std::vector<std::vector<float>>& channels, double& sampleRate,
                       double maxSeconds = kMaxSeconds) {
  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
  if (!reader) return "Could not read " + file.getFileName() + ".";
  sampleRate = reader->sampleRate;
  const juce::int64 from = std::max<juce::int64>(0, (juce::int64)std::llround(fromSeconds * sampleRate));
  juce::int64 frames = reader->lengthInSamples - from;
  if (seconds > 0) frames = std::min<juce::int64>(frames, (juce::int64)std::llround(seconds * sampleRate));
  if (frames <= 0) return "There is no audio there (did the file change?).";
  if (frames > (juce::int64)(maxSeconds * sampleRate)) return "It is longer than " + juce::String((int)(maxSeconds / 60)) + " minutes: cut the part you want first.";
  try {
    juce::AudioBuffer<float> buf((int)reader->numChannels, (int)frames);
    if (!reader->read(&buf, 0, (int)frames, from, true, true)) return "Could not read the audio.";
    channels.clear();
    for (int ch = 0; ch < buf.getNumChannels(); ++ch) channels.emplace_back(buf.getReadPointer(ch), buf.getReadPointer(ch) + frames);
  } catch (const std::bad_alloc&) {
    return "Not enough memory for this audio.";
  }
  return {};
}

// A 24-bit WAV, written next to the target first: an old file stays until the new one is complete.
bool writeWav(const juce::File& target, const std::vector<std::vector<float>>& channels, double sampleRate, int bits = 24) {
  if (channels.empty() || channels[0].empty()) return false;
  target.getParentDirectory().createDirectory();
  juce::TemporaryFile temp(target);
  {
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(temp.getFile());
    if (!static_cast<juce::FileOutputStream*>(out.get())->openedOk()) return false;
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(out, juce::AudioFormatWriterOptions().withSampleRate(sampleRate)
                                                  .withNumChannels((int)channels.size()).withBitsPerSample(bits));
    if (!writer) return false;
    std::vector<const float*> ptrs;
    for (const auto& ch : channels) ptrs.push_back(ch.data());
    if (!writer->writeFromFloatArrays(ptrs.data(), (int)ptrs.size(), (int)channels[0].size())) return false;
  }  // the writer closes the file here
  return temp.overwriteTargetFileWithTemporary();
}

// "Edit · Kick 3" -> "Kick 3": the copy's track keeps the sample's name for the next save.
juce::String withoutEditPrefix(const juce::String& name) {
  return name.startsWith(u8("Edit · ")) ? name.fromFirstOccurrenceOf(u8("Edit · "), false, false) : name;
}

const juce::StringArray& roleList() {
  static const juce::StringArray roles{"kick", "bass", "hat", "openhat", "snare", "clap", "perc", "tom", "crash", "impact",
                                       "riser", "fx", "vox", "pad", "synth", "orchestral", "fill", "kickbass", "other"};
  return roles;
}
}  // namespace

// ---- a region rendered by the engine (an export job to a temporary file)

struct MainComponent::SampleMaker::Render {
  enum class Kind { Sample, Bounce, Resample } kind = Kind::Sample;
  juce::File file;
  std::string path;     // UTF-8, as the export job reports it
  Audio audio;          // all but the channels
  juce::uint32 since = 0;
  std::string trackId;  // Bounce: the source
  int64_t startTick = 0, endTick = 0;
};

MainComponent::SampleMaker::SampleMaker(MainComponent& owner) : o_(owner) {}

MainComponent::SampleMaker::~SampleMaker() {
  stopTimer();
  if (render_) render_->file.deleteFile();
  if (loopWindow_ != nullptr) delete loopWindow_.getComponent();  // a job still writing it fails or writes a file nobody reads (in temp)
}

void MainComponent::SampleMaker::saveClip(const std::string& trackId, const std::string& clipId, double fromSeconds, double toSeconds) {
  const doc::Document& d = o_.history_.current();
  const doc::AudioClip* c = findClip(d, trackId, clipId);
  const doc::Asset* a = c ? d.findAsset(c->assetId) : nullptr;
  if (!c || !a || o_.projectDir().empty()) return;
  if (c->durationSeconds > kMaxSeconds) { o_.showError("Save as sample", "The clip is longer than 10 minutes: cut the part you want first."); return; }
  Audio audio;
  // With VOICE STUTTER's pitch or freeze the clip reads more (or less) of the file than it plays, through the engine's
  // own grains: the sample is what is heard (audit 2026-11-07: it was the file as it is).
  const bool processed = c->pitchSemitones != 0 || c->freezeSeconds > 0;
  const double readSeconds = processed ? core::sampling::clipSourceSeconds(c->durationSeconds, c->pitchSemitones, c->freezeSeconds) : c->durationSeconds;
  const juce::String problem = readAudio(fileOf(doc::assetFile(*a, o_.projectDir())), c->offsetSeconds, readSeconds, audio.channels, audio.sampleRate);
  if (problem.isNotEmpty()) { o_.showError("Save as sample", problem); return; }
  if (processed)
    audio.channels = core::sampling::processClip(audio.channels, audio.sampleRate, (int64_t)std::llround(c->durationSeconds * audio.sampleRate),
                                                 c->pitchSemitones, c->freezeSeconds);
  // As it sounds: the clip's gain and fades over the whole clip, then the part.
  core::sampling::shapeClip(audio.channels, audio.sampleRate, c->gainDb, c->fadeInSeconds, c->fadeOutSeconds);
  bool part = false;
  if (toSeconds > fromSeconds) {
    const int64_t total = (int64_t)audio.channels[0].size();
    const int64_t from = std::clamp<int64_t>(std::llround((fromSeconds - c->offsetSeconds) * audio.sampleRate), 0, total);
    const int64_t to = std::clamp<int64_t>(std::llround((toSeconds - c->offsetSeconds) * audio.sampleRate), from, total);
    if (to - from < 16) { o_.showError("Save as sample", "The selection is too short."); return; }
    for (auto& ch : audio.channels) ch = std::vector<float>(ch.begin() + from, ch.begin() + to);
    part = from > 0 || to < total;
  }
  const doc::Track* t = d.findTrack(trackId);
  const juce::String trackName = t ? withoutEditPrefix(u8(t->name)) : juce::String();
  audio.name = (trackName.isNotEmpty() ? trackName : juce::File::createLegalFileName(u8(a->name)).upToLastOccurrenceOf(".", false, false)) +
               (part ? " (part)" : "");
  audio.fromAsset = a->id;
  audio.what = part ? "Sample (part of a clip)" : "Sample";
  audio.bpm = doc::bpmAt(d, c->startTick);
  // A library sample (or one made from it: reversed, pitched...) keeps what the catalogue says it is.
  const doc::LibrarySample* known = nullptr;
  if (o_.hasSampleIndex_) {
    known = o_.sampleIndex_.findHash(a->sha256);
    if (!known && !a->madeFrom.empty()) known = o_.sampleIndex_.findHash(a->madeFrom);
  }
  if (known) audio.role = known->info.role;
  else if (t) audio.role = doc::trackSampleRole(*t);
  if (audio.role.empty()) audio.role = core::sampling::roleFromName(utf8(audio.name));
  const double seconds = audio.channels[0].size() / audio.sampleRate;
  audio.loop = known ? known->info.loop : seconds >= 4 * 60.0 / std::max(1.0, audio.bpm) * 0.98;  // a bar or more
  ask(std::move(audio));
}

void MainComponent::SampleMaker::renderRegion(const std::string& trackId, int64_t startTick, int64_t endTick) {
  const juce::String title = "Render to sample";
  if (render_ || o_.snap_.exportJob.active()) { o_.showError(title, "An export is running: wait for it to end."); return; }
  if (endTick <= startTick) { o_.showError(title, "Set a loop first (hold Ctrl and drag on the ruler): the loop is the part that is rendered."); return; }
  const doc::Document& d = o_.history_.current();
  if (endTick - startTick > kBar * 64) { o_.showError(title, "The loop is longer than 64 bars: make it shorter."); return; }
  const doc::Track* t = trackId.empty() ? nullptr : d.findTrack(trackId);
  if (!trackId.empty() && !t) return;
  auto r = std::make_unique<Render>();
  r->file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getNonexistentChildFile("core-render-to-sample", ".wav", false);
  r->path = utf8(r->file.getFullPathName());
  r->audio.name = t ? withoutEditPrefix(u8(t->name)) + " (render)" : u8("Mix (render)");
  r->audio.role = t ? doc::trackSampleRole(*t) : std::string();
  if (r->audio.role.empty()) r->audio.role = t ? core::sampling::roleFromName(t->name) : "other";
  r->audio.what = t ? "Render of a track" : "Render of the mix";
  r->audio.bpm = doc::bpmAt(d, startTick);
  r->audio.loop = endTick - startTick >= kBar;
  startRender(std::move(r), trackId, startTick, endTick, false, title);
}

bool MainComponent::SampleMaker::startRender(std::unique_ptr<Render> r, const std::string& trackId, int64_t startTick, int64_t endTick,
                                             bool withoutMasterGain, const juce::String& title) {
  if (render_ || o_.snap_.exportJob.active()) { o_.showError(title, "An export is running: wait for it to end."); return false; }
  r->since = juce::Time::getMillisecondCounter();
  r->trackId = trackId;
  r->startTick = startTick;
  r->endTick = endTick;
  o_.session_->exportFiles({{r->path, trackId, withoutMasterGain}}, startTick, endTick, 32);
  o_.snap_.exportJob = o_.session_->snapshot().exportJob;  // "starting" now, not at the window's next tick
  o_.notice_ = "Rendering " + r->audio.name + "...";
  render_ = std::move(r);
  startTimer(100);
  return true;
}

// The render goes into the song's Audio folder, so the song needs a folder first (as Edit a copy does).
void MainComponent::SampleMaker::bounceTrack(const std::string& trackId) {
  const juce::String title = "Bounce to audio";
  if (o_.projectDir().empty()) {
    o_.notice_ = "Save the song first: the bounce goes into its Audio folder.";
    o_.save(true, [this, trackId](SaveResult r) { if (r != SaveResult::Failed) bounceTrack(trackId); });
    return;
  }
  const doc::Document& d = o_.history_.current();
  if (const std::string why = doc::cannotBounce(d, trackId); !why.empty()) { o_.showError(title, u8(why)); return; }
  const doc::Track* t = d.findTrack(trackId);
  auto r = std::make_unique<Render>();
  r->kind = Render::Kind::Bounce;
  r->audio.name = withoutEditPrefix(u8(t->name)) + " (bounce)";
  r->file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile(juce::File::createLegalFileName(r->audio.name), ".wav", false);
  r->path = utf8(r->file.getFullPathName());
  startRender(std::move(r), trackId, 0, d.lengthTicks, true, title);
}

void MainComponent::SampleMaker::resample(int64_t startTick, int64_t endTick) {
  const juce::String title = "Resample";
  if (endTick <= startTick) { o_.showError(title, "Set a loop first (Ctrl + drag on the ruler): the loop is the part that is resampled."); return; }
  if (o_.projectDir().empty()) {
    o_.notice_ = "Save the song first: the resample goes into its Audio folder.";
    o_.save(true, [this, startTick, endTick](SaveResult r) { if (r != SaveResult::Failed) resample(startTick, endTick); });
    return;
  }
  if (endTick - startTick > kBar * 64) { o_.showError(title, "The loop is longer than 64 bars: make it shorter."); return; }
  auto r = std::make_unique<Render>();
  r->kind = Render::Kind::Resample;
  r->audio.name = "Resample";
  r->file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("Mix resample", ".wav", false);
  r->path = utf8(r->file.getFullPathName());
  startRender(std::move(r), "", startTick, endTick, false, title);
}

void MainComponent::SampleMaker::printed(Render& r) {
  const bool bounce = r.kind == Render::Kind::Bounce;
  const juce::String title = bounce ? "Bounce to audio" : "Resample";
  const std::filesystem::path dir = o_.projectDir();
  if (dir.empty()) { o_.showError(title, "The song has no folder any more: save it and try again."); return; }
  doc::Document next = o_.history_.current();
  doc::Printed p;
  try {
    p = bounce ? doc::placeBounce(next, r.trackId, pathOf(r.file), dir, r.startTick, r.endTick)
               : doc::placeResample(next, pathOf(r.file), dir, r.startTick, r.endTick);
  } catch (const std::exception& e) {
    o_.showError(title, u8(e.what()));
    return;
  }
  const juce::String name = u8(doc::trackById(next, p.trackId).name);
  o_.edit([&](doc::Document& d) { d = next; }, (bounce ? u8("Bounce ") : u8("Resample: ")) + name);
  o_.selectClip(p.trackId, p.clipId);
  o_.notice_ = bounce ? name + u8(" is on a new track below; the original is muted (Undo puts it back).")
                      : name + u8(" is on a new track, muted (the mix already plays it). Unmute it to use it.");
}

void MainComponent::SampleMaker::timerCallback() {
  if (!render_) { stopTimer(); return; }
  const ExportJob& job = o_.snap_.exportJob;
  if (job.path != render_->path) {
    // Another export took the engine's job slot (it should not: ours is refused while one runs).
    if (juce::Time::getMillisecondCounter() - render_->since > 10000) {
      stopTimer();
      render_->file.deleteFile();
      render_.reset();
      o_.showError("Render to sample", "The engine did not start the render.");
    }
    return;
  }
  if (job.active()) return;
  stopTimer();
  std::unique_ptr<Render> r = std::move(render_);
  if (job.state != "succeeded") {
    r->file.deleteFile();
    if (job.state == "failed") o_.showError("Render to sample", u8(job.error));
    else o_.notice_ = "Render cancelled.";
    return;
  }
  if (r->kind != Render::Kind::Sample) {
    o_.notice_.clear();
    printed(*r);
    r->file.deleteFile();  // copied into the song's Audio folder (or failed: nothing to keep)
    return;
  }
  Audio audio = std::move(r->audio);
  const juce::String problem = readAudio(r->file, 0, 0, audio.channels, audio.sampleRate);
  r->file.deleteFile();
  if (problem.isNotEmpty()) { o_.showError("Render to sample", problem); return; }
  o_.notice_.clear();
  ask(std::move(audio));
}

void MainComponent::SampleMaker::saveFile(const juce::File& file, const juce::String& name, const std::string& fromAsset, double fromSeconds,
                                         double toSeconds) {
  Audio audio;
  const bool part = toSeconds > fromSeconds;
  const juce::String problem = readAudio(file, part ? fromSeconds : 0, part ? toSeconds - fromSeconds : 0, audio.channels, audio.sampleRate);
  if (problem.isNotEmpty()) { o_.showError("Save as sample", problem); return; }
  audio.name = name;
  audio.role = core::sampling::roleFromName(utf8(name));
  audio.fromAsset = fromAsset;
  audio.what = "File";
  audio.bpm = doc::bpmAt(o_.history_.current(), 0);
  audio.loop = true;
  ask(std::move(audio));
}

void MainComponent::SampleMaker::saveAsset(const std::string& assetId) {
  const doc::Asset* a = o_.history_.current().findAsset(assetId);
  if (!a || o_.projectDir().empty()) return;
  saveFile(fileOf(doc::assetFile(*a, o_.projectDir())), juce::File::createLegalFileName(u8(a->name)).upToLastOccurrenceOf(".", false, false), assetId);
}

void MainComponent::SampleMaker::assetToTrack(const std::string& assetId) {
  const doc::Asset* a = o_.history_.current().findAsset(assetId);
  if (!a || o_.projectDir().empty()) return;
  placeOnNewTrack(fileOf(doc::assetFile(*a, o_.projectDir())), juce::File::createLegalFileName(u8(a->name)).upToLastOccurrenceOf(".", false, false),
                  o_.history_.current().loop.enabled ? o_.history_.current().loop.startTick : (int64_t)o_.snap_.positionTick / kBar * kBar,
                  "Put on a new track: ");
}

// The file on a new audio track at `at` (copied into the song by content: one already there is used again), the
// clip chosen so the editor opens. One undo step.
bool MainComponent::SampleMaker::placeOnNewTrack(const juce::File& file, const juce::String& name, int64_t at, const juce::String& label) {
  const std::filesystem::path dir = o_.projectDir();
  if (dir.empty()) return false;
  doc::Document next = o_.history_.current();
  std::string trackId, clipId;
  try {
    const std::string assetId = doc::importAudio(next, pathOf(file), dir);
    trackId = doc::addAudioTrack(next, utf8(name));
    clipId = doc::addAudioClip(next, trackId, assetId, at);
    for (const auto& c : doc::trackById(next, trackId).audioClips)
      if (c.id == clipId && doc::audioClipEndTick(next, c) > next.lengthTicks)
        doc::setSongLength(next, (doc::audioClipEndTick(next, c) + kBar - 1) / kBar * kBar);
  } catch (const std::exception& e) {
    o_.showError(label.upToLastOccurrenceOf(":", false, false), u8(e.what()));
    return false;
  }
  o_.edit([&](doc::Document& d) { d = next; }, label + name);
  o_.selectClip(trackId, clipId);
  return true;
}

void MainComponent::SampleMaker::editLibrarySample(const doc::LibrarySample& sample) {
  const std::filesystem::path dir = o_.projectDir();
  const std::string file = sample.file;
  const std::string name = sample.displayName.empty() ? sample.name : sample.displayName;
  if (dir.empty()) {  // the copy goes into the song's folder: it needs one first
    o_.notice_ = "Save the song first: the copy to edit goes into its Audio folder.";
    o_.save(true, [this, file](SaveResult r) {
      if (r == SaveResult::Failed || !o_.hasSampleIndex_) return;
      if (const doc::LibrarySample* s = o_.sampleIndex_.findFile(file)) editLibrarySample(*s);
    });
    return;
  }
  const doc::Document& d = o_.history_.current();
  const int64_t at = d.loop.enabled ? d.loop.startTick : (int64_t)o_.snap_.positionTick / kBar * kBar;
  if (!placeOnNewTrack(fileOf(std::filesystem::u8path(file)), u8("Edit · ") + u8(name), at, "Edit a copy: ")) return;
  o_.notice_ = u8("A copy of ") + u8(name) + u8(" is on a new track (the library's file does not change). Edit it below, then More > Save as sample.");
}

// ---- a sampler zone's loop

class MainComponent::SampleMaker::LoopPanel final : public juce::Component, private juce::Timer {
 public:
  LoopPanel(MainComponent& owner, std::string trackId, size_t zone) : o_(owner), trackId_(std::move(trackId)), zone_(zone) {
    if (const doc::SampleRef* z = ref()) {
      file_ = fileOf(std::filesystem::u8path(z->file));
      sha_ = z->sha256.empty() ? z->file : z->sha256;
      juce::AudioFormatManager formats;
      formats.registerBasicFormats();
      // One channel (the left, or the only one) for the zero crossings; at most 10 minutes.
      if (std::unique_ptr<juce::AudioFormatReader> r{formats.createReaderFor(file_)}) {
        rate_ = r->sampleRate;
        const juce::int64 frames = std::min<juce::int64>(r->lengthInSamples, (juce::int64)(rate_ * kMaxSeconds));
        seconds_ = frames / rate_;
        juce::AudioBuffer<float> buf(1, (int)std::max<juce::int64>(1, frames));
        if (frames > 0 && r->read(&buf, 0, (int)frames, 0, true, false)) left_.assign(buf.getReadPointer(0), buf.getReadPointer(0) + frames);
      }
    }
    for (auto* b : {&off_, &whole_, &snap_, &hear_}) { b->setWantsKeyboardFocus(false); addAndMakeVisible(b); }
    off_.setButtonText("Loop off");
    whole_.setButtonText("Whole sample");
    snap_.setButtonText("Snap to zero crossings");
    snap_.setToggleState(true, juce::dontSendNotification);
    snap_.setClickingTogglesState(true);
    snap_.setColour(juce::TextButton::buttonOnColourId, kPrimary);
    hear_.setButtonText("Hold to hear");
    hear_.setTooltip("Plays the zone's note while held (the loop plays until you let go)");
    off_.onClick = [this] { commit(0, 0); };
    whole_.onClick = [this] { if (seconds_ > 0) commit(0, seconds_ - 1.0 / rate_); };
    hear_.addMouseListener(this, false);
    setSize(820, 300);
    startTimerHz(15);  // the waveform arrives, the song changes (undo)
  }
  ~LoopPanel() override { if (hearing_) o_.session_->liveNote(trackId_, key(), 0, false); }

  void paint(juce::Graphics& g) override {
    g.fillAll(kPanel);
    const auto wave = waveArea();
    g.setColour(kField);
    g.fillRoundedRectangle(wave.toFloat(), theme::kCorner);
    const doc::SampleRef* z = ref();
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    g.drawText(lang::tr(z ? u8(z->name) : juce::String("The zone is gone")), getLocalBounds().reduced(12, 8).removeFromTop(20), juce::Justification::centredLeft);
    if (!z || seconds_ <= 0) {
      g.setColour(kMuted);
      g.drawText(lang::tr(file_.existsAsFile() ? "Could not read the sample." : "The sample's file is missing."), wave, juce::Justification::centred);
      return;
    }
    const double a = dragging_ ? from_ : z->loopStart, b = dragging_ ? to_ : z->loopEnd;
    if (b > a) {  // the loop, lit
      g.setColour(kAccent.withAlpha(0.18f));
      g.fillRect(juce::Rectangle<float>(xOf(a), (float)wave.getY(), xOf(b) - xOf(a), (float)wave.getHeight()));
    }
    const auto w = o_.waves_.get(sha_, file_);
    const float mid = (float)wave.getCentreY(), half = wave.getHeight() / 2.0f - 4;
    g.setColour(kMuted);
    if (w) drawWaveform(g, *w, wave, 0.0, seconds_, kAccent, 4.0f);
    if (b > a)
      for (double e : {a, b}) {
        g.setColour(kAccentBright);
        g.fillRect(xOf(e) - 1.0f, (float)wave.getY(), 2.0f, (float)wave.getHeight());
        g.fillRect(xOf(e) - 5.0f, (float)wave.getY(), 10.0f, 6.0f);  // a handle to grab
      }
    g.setColour(kMuted);
    g.setFont(juce::FontOptions(theme::kFontBody));
    const juce::String text = b > a ? "Loop " + juce::String(a, 3) + " s to " + juce::String(b, 3) + " s (" + juce::String((b - a) * 1000.0, 1) +
                                          " ms): plays while the note is held, then the release."
                                    : juce::String("No loop: the sample plays once. Drag across the waveform to make one.");
    g.drawText(text, juce::Rectangle<int>(wave.getX(), wave.getBottom() + 4, wave.getWidth(), 18), juce::Justification::centredLeft);
  }

  void resized() override {
    auto row = getLocalBounds().reduced(12, 8).removeFromBottom(28);
    off_.setBounds(row.removeFromLeft(90).reduced(2, 0));
    whole_.setBounds(row.removeFromLeft(120).reduced(2, 0));
    snap_.setBounds(row.removeFromLeft(180).reduced(2, 0));
    hear_.setBounds(row.removeFromRight(120).reduced(2, 0));
  }

  void mouseDown(const juce::MouseEvent& e) override {
    if (e.eventComponent == &hear_) { hearing_ = true; o_.session_->liveNote(trackId_, key(), 1.0, true); return; }
    const doc::SampleRef* z = ref();
    if (!z || seconds_ <= 0 || !waveArea().contains(e.getPosition())) return;
    from_ = z->loopStart;
    to_ = z->loopEnd;
    const double at = secondsAt(e.x);
    // An edge near the mouse moves; elsewhere a new loop is drawn from here.
    if (to_ > from_ && std::abs(e.x - xOf(from_)) <= 8) grab_ = 1;
    else if (to_ > from_ && std::abs(e.x - xOf(to_)) <= 8) grab_ = 2;
    else { grab_ = 3; anchor_ = at; from_ = to_ = at; }
    dragging_ = true;
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (!dragging_) return;
    const double at = secondsAt(e.x);
    if (grab_ == 1) from_ = std::min(at, to_ - doc::kMinSampleLoopSeconds);
    else if (grab_ == 2) to_ = std::max(at, from_ + doc::kMinSampleLoopSeconds);
    else { from_ = std::min(anchor_, at); to_ = std::max(anchor_, at); }
    repaint();
  }
  void mouseUp(const juce::MouseEvent& e) override {
    if (e.eventComponent == &hear_) { if (hearing_) o_.session_->liveNote(trackId_, key(), 0, false); hearing_ = false; return; }
    if (!dragging_) return;
    dragging_ = false;
    if (to_ - from_ >= doc::kMinSampleLoopSeconds) commit(from_, to_);
    repaint();
  }

 private:
  const doc::SampleRef* ref() const {
    const doc::Track* t = o_.history_.current().findTrack(trackId_);
    return t && zone_ < t->instrument.samples.size() ? &t->instrument.samples[zone_] : nullptr;
  }
  int key() const {
    const doc::SampleRef* z = ref();
    return z ? std::clamp(z->rootKey, z->loKey, z->hiKey) : 60;
  }
  juce::Rectangle<int> waveArea() const { return getLocalBounds().reduced(12, 8).withTrimmedTop(28).withTrimmedBottom(28 + 26); }
  double secondsAt(int x) const {
    const auto w = waveArea();
    return std::clamp(seconds_ * (x - w.getX()) / std::max(1, w.getWidth()), 0.0, seconds_);
  }
  float xOf(double s) const {
    const auto w = waveArea();
    return (float)(w.getX() + s / std::max(1e-9, seconds_) * w.getWidth());
  }
  // The nearest rising zero crossing (within 10 ms), so both edges meet the wave the same way up.
  double snapped(double s) const {
    if (!snap_.getToggleState() || left_.size() < 2) return s;
    const int64_t at = (int64_t)std::llround(s * rate_), reach = (int64_t)(0.010 * rate_), n = (int64_t)left_.size();
    for (int64_t d = 0; d <= reach; ++d)
      for (int64_t i : {at + d, at - d})
        if (i > 0 && i < n && left_[(size_t)i - 1] < 0 && left_[(size_t)i] >= 0) return i / rate_;
    return s;
  }
  void commit(double a, double b) {
    if (b > 0) { a = snapped(a); b = snapped(b); }
    if (b > 0 && b < a + doc::kMinSampleLoopSeconds) return;
    const std::string t = trackId_;
    const size_t zone = zone_;
    const double length = seconds_;
    o_.edit([&](doc::Document& d) { doc::setSampleLoop(d, t, zone, a, b, length); }, b > 0 ? "Sample loop" : "Sample loop off");
    repaint();
  }
  void timerCallback() override { if (!dragging_) repaint(); }

  MainComponent& o_;
  std::string trackId_;
  size_t zone_;
  juce::File file_;
  std::string sha_;
  double rate_ = 48000, seconds_ = 0;
  std::vector<float> left_;
  juce::TextButton off_, whole_, snap_, hear_;
  bool dragging_ = false, hearing_ = false;
  int grab_ = 0;
  double from_ = 0, to_ = 0, anchor_ = 0;
};

void MainComponent::SampleMaker::editLoop(const std::string& trackId, size_t zone) {
  const doc::Track* t = o_.history_.current().findTrack(trackId);
  if (!t || zone >= t->instrument.samples.size()) return;
  juce::DialogWindow::LaunchOptions options;
  options.content.setOwned(new LoopPanel(o_, trackId, zone));
  options.dialogTitle = "Sample loop: " + u8(t->name);
  options.dialogBackgroundColour = kPanel;
  options.escapeKeyTriggersCloseButton = true;
  options.useNativeTitleBar = true;
  options.resizable = true;
  options.componentToCentreAround = &o_;
  if (loopWindow_ != nullptr) delete loopWindow_.getComponent();  // one at a time
  loopWindow_ = options.launchAsync();
}

// ---- the dialog

void MainComponent::SampleMaker::ask(Audio audio) {
  const double seconds = audio.channels.empty() ? 0 : audio.channels[0].size() / audio.sampleRate;
  const juce::File library = o_.sampleLibraryFolder();
  juce::String message;
  message << juce::String(seconds, 2) << " s, " << juce::String((int)audio.channels.size()) << (audio.channels.size() == 1 ? " channel" : " channels")
          << ", " << juce::String(audio.sampleRate / 1000.0, 1) << " kHz.";
  auto* w = new juce::AlertWindow(lang::tr("Save as sample"), lang::tr(message), juce::MessageBoxIconType::NoIcon, &o_);
  w->addTextEditor("name", audio.name, "Name:");
  w->addComboBox("role", roleList(), "Role:");
  w->getComboBoxComponent("role")->setSelectedItemIndex(std::max(0, roleList().indexOf(u8(audio.role.empty() ? "other" : audio.role))));
  w->addComboBox("kind", {"one-shot", "loop"}, "Kind:");
  w->getComboBoxComponent("kind")->setSelectedItemIndex(audio.loop ? 1 : 0);
  w->addTextEditor("bpm", audio.bpm > 0 ? juce::String(audio.bpm, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".") : juce::String(),
                   "BPM (a loop's tempo; empty: none):");
  juce::StringArray where;
  where.add(library.isDirectory() ? "Sample library (" + library.getFullPathName() + "\\Core\\<role>)" : juce::String("Sample library (none chosen: Samples menu)"));
  where.add(o_.projectDir().empty() ? juce::String("This project (save the song first)") : juce::String("This project (the song's Audio folder; Library > Project)"));
  where.add(u8("Another folder…"));
  w->addComboBox("where", where, "Save to:");
  w->getComboBoxComponent("where")->setSelectedItemIndex(library.isDirectory() ? 0 : o_.projectDir().empty() ? 2 : 1);
  w->addComboBox("silence", {"Cut the silence at the start and end", "Keep it as it is"}, "Silence:");
  w->getComboBoxComponent("silence")->setSelectedItemIndex(0);
  w->addComboBox("level", {"Keep the level", "Normalize (peak at -0.3 dB)"}, "Level:");
  w->getComboBoxComponent("level")->setSelectedItemIndex(0);
  w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> safe(w);
  auto held = std::make_shared<Audio>(std::move(audio));
  w->enterModalState(true, juce::ModalCallbackFunction::create([this, safe, held](int result) {
    if (result != 1 || safe == nullptr) return;
    Choice c;
    c.name = safe->getTextEditorContents("name").trim();
    if (c.name.isEmpty()) c.name = held->name;
    c.role = utf8(safe->getComboBoxComponent("role")->getText());
    c.loop = safe->getComboBoxComponent("kind")->getSelectedItemIndex() == 1;
    c.bpm = safe->getTextEditorContents("bpm").replaceCharacter(',', '.').getDoubleValue();
    if (!(c.bpm >= 40 && c.bpm <= 999)) c.bpm = 0;
    c.destination = (Destination)(safe->getComboBoxComponent("where")->getSelectedItemIndex() + 1);
    c.trim = safe->getComboBoxComponent("silence")->getSelectedItemIndex() == 0;
    c.normalize = safe->getComboBoxComponent("level")->getSelectedItemIndex() == 1;
    if (c.destination != Destination::Folder) { save(std::move(*held), c); return; }
    const juce::File start = o_.sampleLibraryFolder().isDirectory() ? o_.sampleLibraryFolder()
                                                                      : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    o_.chooser_ = std::make_unique<juce::FileChooser>("Save the sample (WAV)", start.getChildFile(u8(core::sampling::safeFileName(utf8(c.name))) + ".wav"), "*.wav");
    o_.chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                             [this, held, c](const juce::FileChooser& fc) mutable {
      if (fc.getResult() == juce::File()) return;
      c.folder = fc.getResult().hasFileExtension("wav") ? fc.getResult() : fc.getResult().withFileExtension("wav");
      save(std::move(*held), c);
    });
  }), true);
}

juce::File MainComponent::SampleMaker::libraryFile(const Choice& c) const {
  const juce::File library = o_.sampleLibraryFolder();
  if (!library.isDirectory()) return {};
  return library.getChildFile("Core").getChildFile(u8(doc::sampleRoleLabel(c.role)))
      .getChildFile(u8(core::sampling::safeFileName(utf8(c.name))) + ".wav").getNonexistentSibling(false);
}

juce::File MainComponent::SampleMaker::save(Audio audio, const Choice& c) {
  const juce::String title = "Save as sample";
  if (c.trim && !core::sampling::trimSilence(audio.channels, audio.sampleRate, -60, 2)) { o_.showError(title, "It is silent: nothing was saved."); return {}; }
  if (c.normalize) core::sampling::normalizePeak(audio.channels, -0.3);
  if (audio.channels.empty() || audio.channels[0].empty()) { o_.showError(title, "There is no audio: nothing was saved."); return {}; }

  if (c.destination == Destination::Project) {
    const std::filesystem::path dir = o_.projectDir();
    if (dir.empty()) { o_.showError(title, "Save the song first: its samples go into its folder."); return {}; }
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("core-sample", ".wav", false);
    if (!writeWav(temp, audio.channels, audio.sampleRate)) { o_.showError(title, "Could not write the sample."); return {}; }
    std::string id;
    o_.edit([&](doc::Document& d) { id = doc::saveProjectSample(d, pathOf(temp), dir, utf8(c.name), audio.fromAsset, utf8(audio.what)); },
            "Save sample to this project: " + c.name);  // a failure is shown by edit (nothing changes)
    temp.deleteFile();
    if (id.empty()) return {};
    const doc::Asset* a = o_.history_.current().findAsset(id);
    o_.notice_ = "Saved in this project: " + c.name + " (Library > Project). Save the song to keep it.";
    return a ? fileOf(doc::assetFile(*a, dir)) : juce::File();
  }

  const juce::File target = c.destination == Destination::Library ? libraryFile(c) : c.folder;
  if (target == juce::File()) { o_.showError(title, "No sample library folder: choose one in the Samples menu (or save to another folder)."); return {}; }
  if (!writeWav(target, audio.channels, audio.sampleRate)) { o_.showError(title, "Could not write " + target.getFullPathName() + "."); return {}; }
  o_.notice_ = "Saved the sample: " + target.getFullPathName();
  if (c.destination != Destination::Library) return target;

  // Into the Library at once, with what the dialog said as the user's fields (they follow the content).
  if (!o_.hasSampleIndex_) {
    o_.sampleIndex_.root = utf8(o_.sampleLibraryFolder().getFullPathName());
    o_.sampleIndex_.samples.clear();
    o_.hasSampleIndex_ = true;
  }
  try {
    const std::string sha = doc::addLibraryFile(o_.sampleIndex_, pathOf(target)).sha256;
    doc::json fields = {{"name", utf8(c.name)}, {"role", c.role}, {"kind", c.loop ? "loop" : "one-shot"}, {"tags", {"core"}}};
    if (c.loop && c.bpm > 0) fields["bpm"] = c.bpm;
    doc::setSampleMetadata(o_.sampleIndex_, {sha}, fields, "save as sample " + utf8(c.name));
  } catch (const std::exception& e) {
    logLine("Save as sample: not in the catalogue yet (" + u8(e.what()) + "); the next scan reads it.");
  }
  o_.sampleCatalogChanged();
  o_.library_.indexChanged();
  o_.startSampleAnalysis();
  if (o_.sampleIndex_.samples.size() <= 1) o_.scanSampleLibrary();  // the index was new: read the rest of the library
  return target;
}

}  // namespace core::app
