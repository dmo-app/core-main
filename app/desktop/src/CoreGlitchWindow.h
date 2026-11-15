// Core Glitch · Sample Path in its own window (Diogo, 2026-11-08/09: every plugin opens a window of its own, several at
// once). One window per Core Glitch track; opening one that is open brings it to the front.
//
// What it shows (his mockup, 2026-11-09): a header with Host Sync; tabs Sampler 1-4, each with its own colour, which
// recolours the window; cards Node (the chosen node's knobs), Sample · Slices (the sound cut in coloured tiles) and Node
// FX (the sampler's two slots); the Sample Path map (small round nodes, colour only, white numbers; links in the colour
// of the node they leave; the ring that walks while the song plays); Path (rate, variation, seed, mode, end, shapes, the
// chosen node's exits, Lock Path); Global (amp, pitch, reverb, sync, limiter, mute); and a bar of node and pattern
// buttons, Generate Variation, Start here and the fade. Every change is one undo step of the song (a drag is one).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"

namespace core::doc { struct SampleIndex; }

namespace core::app {

class CoreGlitchWindows final : private juce::Timer {
public:
  struct Callbacks {
    std::function<void()> undo, redo;
    std::function<const doc::SampleIndex*()> sampleLibrary;  // null: the library has not been read
  };
  CoreGlitchWindows(PatternEditor::Host& host, Callbacks callbacks);
  ~CoreGlitchWindows() override;

  void open(const std::string& trackId);  // brings it to the front when it is open
  void documentChanged();                 // the windows follow the song (undo too); a window whose track is gone closes
  void closeAll();
  int openCount() const { return (int)windows_.size(); }

  class Editor;
  class Window;

private:
  void timerCallback() override;
  void close(Window* w);

  PatternEditor::Host& host_;
  Callbacks cb_;
  std::vector<std::unique_ptr<Window>> windows_;
};

} // namespace core::app
