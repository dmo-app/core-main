// Core FX and Core EQ (2026-11-09) in their own windows (Diogo, 2026-11-08: "todos os plugins glitch, synth, fx abre uma janela e podemos ter
// varias abertas"). One window per Core FX insert, as many open as you like; opening one that is open brings it to
// the front. The window in front shows its effect's input spectrum (the engine's analysis.watch).
//
// What it shows (the brief, 2026-11-08): presets, undo/redo, A/B, bypass, Amount, FX Only, limiter and Output on
// top; the regions of the five modules as curves over the spectrum (drag a node: frequency and amount; the wheel:
// width; double-click: add or remove a region); a card per module (On, Solo, Type, its two knobs, Return, Fit,
// lock); two LFOs in time with the song whose destinations are listed under them. Vary changes what is not locked.
// Every change is one undo step of the song (a drag is one gesture).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"

namespace core::app {

class CoreFxWindows final : private juce::Timer {
public:
  struct Callbacks {
    std::function<void()> undo, redo;
    // The effect whose input the engine measures: owner = track id or "master"; effectId "" stops.
    std::function<void(const std::string& owner, const std::string& effectId)> watch;
    // The watched effect's input: 1/6-octave bands from 20 Hz, dB (empty: none yet).
    std::function<std::vector<float>()> spectrum;
  };
  CoreFxWindows(PatternEditor::Host& host, Callbacks callbacks);
  ~CoreFxWindows() override;

  // trackId "" = the master chain; a Core FX or a Core EQ. Brings it to the front when it is open.
  void open(const std::string& trackId, const std::string& effectId);
  void documentChanged();  // the windows follow the song (undo too); a window whose effect is gone closes
  void closeAll();
  int openCount() const { return (int)windows_.size(); }

  class Pane;    // an editor tied to one effect (CoreFxPane.h)
  class Lfo;
  class Editor;  // Core FX's
  class Window;

private:
  void timerCallback() override;
  void activated(Window* w);
  void close(Window* w);
  void watch(Window* w);

  PatternEditor::Host& host_;
  Callbacks cb_;
  std::vector<std::unique_ptr<Window>> windows_;
  Window* watched_ = nullptr;
};

} // namespace core::app
