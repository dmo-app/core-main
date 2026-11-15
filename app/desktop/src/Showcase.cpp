// Core.exe --showcase <folder>: pictures of every page with a full song, for comparing with the reference
// images. The dark psycore template (with the library's samples when there is a library) plays; the
// pictures are taken while it plays: the arrangement and the Mixer. Nothing the user has is changed: the song
// is a new one, saved only inside <folder>.
#include "MainComponent.h"
#include "Mixer.h"

namespace core::app {

namespace {
juce::String u8s(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }
}  // namespace

void MainComponent::runShowcase(juce::File folder, std::function<void(bool, juce::String)> done) {
  showcase_ = std::make_unique<Showcase>();
  showcase_->folder = std::move(folder);
  showcase_->done = std::move(done);
  showcase_->started = showcase_->stepStarted = juce::Time::getMillisecondCounterHiRes();
  showcase_->folder.createDirectory();
}

void MainComponent::showcaseStep() {
  Showcase& s = *showcase_;
  const double now = juce::Time::getMillisecondCounterHiRes(), inStep = now - s.stepStarted;
  const bool applied = snap_.appliedRevision == snap_.documentRevision;
  const bool loud = std::max(snap_.masterPeak[0], snap_.masterPeak[1]) > 0.02f;
  auto next = [&](const juce::String& what) { s.log << what << "; "; ++s.step; s.stepStarted = now; };
  auto finish = [&](bool ok, const juce::String& why) {
    auto done = std::move(s.done);
    const juce::String report = juce::String(ok ? "ok" : "failed") + ": " + why + " | steps: " + s.log + "| status=" + statusText(snap_.status) +
                                " message=" + u8s(snap_.message);
    showcase_.reset();
    done(ok, report);
  };
  auto picture = [&](const char* name) {
    const juce::File f = s.folder.getChildFile(name);
    if (!savePicture(f)) { finish(false, "cannot write " + f.getFullPathName()); return false; }
    return true;
  };
  if (now - s.started > 240000) return finish(false, "timeout at step " + juce::String(s.step));
  if (snap_.status == EngineStatus::Stopped || snap_.status == EngineStatus::Diverged) return finish(false, "engine problem");
  switch (s.step) {
    case 0:  // a new song from the template (and the library's samples), the bass chosen, from the groove
      if (snap_.status != EngineStatus::Ready || inStep < 500) break;
      reloadTemplates();
      {
        // The templates play Core's own instruments and samples (the built-in references are test fixtures since 2026-11-05).
        // A template the library shows: darkpsycore and the other bases are hidden from it (audit of main a3d9f14, F2).
        std::string id;
        for (const char* want : {"hitech-ref", "slambient-ref"})
          if (id.empty() && templates_.find(want)) id = want;
        if (id.empty()) return finish(false, "no hitech-ref or slambient-ref template");
        newFromTemplate(id);
      }
      next("template");
      break;
    case 1: {
      if (!applied || inStep < 1500) break;
      const doc::Document& d = history_.current();
      for (const auto& t : d.tracks) if (s.bass.empty() && juce::String(u8s(t.name)).containsIgnoreCase("bass")) s.bass = t.id;
      if (s.bass.empty() && !d.tracks.empty()) s.bass = d.tracks.front().id;
      for (const auto& sec : d.sections) {
        if (sec.name == "Groove" || sec.name == "Main" || sec.name == "Layers") { s.from = sec.startTick; s.to = sec.startTick + std::min<int64_t>(sec.lengthTicks, 32 * 4 * doc::kPpq); }
      }
      if (s.to <= s.from) { s.from = 0; s.to = std::min<int64_t>(d.lengthTicks, 32 * 4 * doc::kPpq); }
      selectTrack(s.bass);
      session_->seek(s.from + 4 * 4 * doc::kPpq);
      session_->play();
      next("playing");
      break;
    }
    case 2:  // the arrangement, playing
      if (snap_.playing && loud && inStep > 4000) {
        if (!picture("1-arrange.png")) return;
        setMixerOpen(true);
        selectTrack(s.bass);
        if (mixer_) mixer_->modulation().addLfo();  // Modulation under the strips shows an LFO (ref-3)
        // The bass's compressor keyed by the kick (the Sidechain row on its card, ref-3).
        applyEdit([&s](doc::Document& d) {
          std::string kick;
          for (const auto& t : d.tracks) if (kick.empty() && juce::String(u8s(t.name)).containsIgnoreCase("kick")) kick = t.id;
          const auto& chain = doc::effectChain(d, s.bass);
          for (size_t i = 0; i < chain.size() && !kick.empty(); ++i)
            if (doc::effectName(chain[i]) == "compressor") { doc::setEffectSidechain(d, s.bass, (int)i, kick); break; }
        }, "Sidechain");
        next("arrange");
      }
      break;
    case 3:  // the Mixer: meters, CPU (asked once a second), the bass's inserts
      if (snap_.playing && inStep > 3500) {
        if (!picture("2-mixer.png")) return;
        setMixerOpen(false);
        // The page of a built-in instrument with a filter (the kick, a Sampler): sound and modulation together,
        // an LFO on Cutoff and an envelope on Resonance, moving while it plays.
        const doc::Document& d = history_.current();
        for (const auto& t : d.tracks)
          if (s.sound.empty() && t.instrument.kind == "builtin" && doc::findInstrumentParam(t, "cutoff") && doc::findInstrumentParam(t, "filterType")) s.sound = t.id;
        if (s.sound.empty()) {
          session_->stop();
          return finish(true, "2 pictures in " + s.folder.getFullPathName() + " (no instrument with a filter for the sound page)");
        }
        applyEdit([&s](doc::Document& doc) {
          doc::setInstrumentParam(doc, s.sound, "filterType", 1);
          doc::setInstrumentParam(doc, s.sound, "cutoff", 900);
          doc::setInstrumentParam(doc, s.sound, "resonance", 0.6);
        }, "Filter");
        openSound(s.sound, "cutoff", 0);
        openSound(s.sound, "resonance", 1);
        next("sound page");
      }
      break;
    case 4:  // the sound page, playing: the filter's curve and the dots move
      if (snap_.playing && inStep > 2500) {
        if (!picture("3-sound.png")) return;
        s.stepStarted = now - 2500 + 180;  // a second picture a moment later: the curve has moved
        ++s.step;
      }
      break;
    case 5:
      if (inStep > 2500) {
        if (!picture("4-sound-later.png")) return;
        closePanels();
        session_->stop();
        return finish(true, "4 pictures in " + s.folder.getFullPathName());
      }
      break;
  }
}

}  // namespace core::app
