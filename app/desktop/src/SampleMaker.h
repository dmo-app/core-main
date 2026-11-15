// Making samples in the window: a clip (or the part of it chosen in the audio editor), a region of the song
// rendered by the engine (one track after its effects and fader, or the whole mix), one of the song's audio files.
// One dialog asks for the name, the catalogue's role, one-shot or loop, and where it goes:
//   - the sample library (D:\Samples by default), in Core\<Role>\: the file joins the Library at once, with
//     what the dialog says as the user's catalogue fields (tag "core");
//   - this project: one of the song's audio files (Library > Project), kept with no clip; one undo step;
//   - another folder: a plain WAV.
// Optional: cut the silence at the start and end, normalize to -0.3 dBFS peak. Files are 24-bit WAV at the
// source's rate; nothing in the song changes before the user says where it goes, and the source never changes.
// Also "Edit a library sample": the sample on a new audio track of the song, so the audio editor (trim, fades,
// reverse, pitch, tempo...) works on it, then Save as sample makes the new one (the library's file stays).
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "MainComponent.h"

namespace core::app {

class MainComponent::SampleMaker : private juce::Timer {
 public:
  explicit SampleMaker(MainComponent& owner);
  ~SampleMaker() override;  // a render still running: its file is deleted when it ends

  // The clip as it sounds (gain, fades). fromSeconds/toSeconds: a part of it in the file's seconds (the audio
  // editor's selection); toSeconds <= fromSeconds: the whole clip.
  void saveClip(const std::string& trackId, const std::string& clipId, double fromSeconds = 0, double toSeconds = 0);
  // [startTick, endTick) of the song, rendered by the engine: one track (after its effects and fader, before the
  // master) or, with trackId "", the mix (after the master chain). The song's tail (2 s) comes too; the
  // dialog's "cut silence" takes what is quiet away.
  void renderRegion(const std::string& trackId, int64_t startTick, int64_t endTick);
  // A WAV file (one of the song's audio files): all of it, or [fromSeconds, toSeconds) when toSeconds > fromSeconds.
  // fromAsset: the song asset it comes from, or "".
  void saveFile(const juce::File& file, const juce::String& name, const std::string& fromAsset, double fromSeconds = 0, double toSeconds = 0);
  // Library > Project > right click: one of the song's audio files on a new track, or saved as a sample.
  void assetToTrack(const std::string& assetId);
  void saveAsset(const std::string& assetId);
  // Library > right click > Edit a copy: the sample on a new audio track, chosen (the editor opens below).
  void editLibrarySample(const doc::LibrarySample& sample);
  // Bounce (AB1): the whole track as it is heard (instrument, effects, fader, pan, its buses and sends, without the
  // master) on a new audio track right below it; the track is muted, so the song sounds the same. One undo step.
  void bounceTrack(const std::string& trackId);
  // Resample (AB1): the mix of [startTick, endTick) on a new audio track at startTick, muted (the mix already plays
  // it): material to cut, reverse and process again. One undo step.
  void resample(int64_t startTick, int64_t endTick);
  bool rendering() const { return render_ != nullptr; }
  // A sampler zone's sustain loop, in a window of its own: the sample's waveform with the loop's two edges to
  // drag (snapped to zero crossings), Off, Whole sample, and Hold to hear. Each change is one undo step.
  void editLoop(const std::string& trackId, size_t zone);

  // What the dialog asks (also the self-test's way in, without the dialog).
  enum class Destination { Library = 1, Project = 2, Folder = 3 };
  struct Audio {
    std::vector<std::vector<float>> channels;
    double sampleRate = 48000;
    juce::String name;          // suggested
    std::string role;           // suggested ("" = from the name)
    std::string fromAsset;      // the song's asset it was made from ("" = none)
    juce::String what;          // how ("Clip", "Render", "File")
    double bpm = 0;             // the song's tempo where it was taken (for a loop)
    bool loop = false;          // suggested kind
  };
  struct Choice {
    juce::String name;
    std::string role = "other";
    bool loop = false;
    double bpm = 0;
    Destination destination = Destination::Library;
    bool trim = true, normalize = false;
    juce::File folder;          // Destination::Folder: the file itself
  };
  // Writes the sample; returns the file written ("" on failure, the reason shown).
  juce::File save(Audio audio, const Choice& choice);

 private:
  struct Render;
  class LoopPanel;
  void ask(Audio audio);
  void timerCallback() override;
  juce::File libraryFile(const Choice& choice) const;
  bool placeOnNewTrack(const juce::File& file, const juce::String& name, int64_t at, const juce::String& label);
  // Starts the engine's render of one file into render_ (false: refused, the reason shown).
  bool startRender(std::unique_ptr<Render> r, const std::string& trackId, int64_t startTick, int64_t endTick, bool withoutMasterGain,
                   const juce::String& title);
  void printed(Render& r);  // a bounce or resample that finished: into the song

  MainComponent& o_;
  std::unique_ptr<Render> render_;
  juce::Component::SafePointer<juce::DialogWindow> loopWindow_;  // closed with the window (it uses the song)
};

}  // namespace core::app
