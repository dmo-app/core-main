// W-Core's own window, as the design Diogo approved (2026-11-09, "esta ja feito"): every plugin opens in a window of
// its own, several at once. A band with CORE · W-CORE SYNTH (Pista and A/B on its right); the tabs OSC and MATRIX
// with Panels (the other Core windows) and Lead/Pad; the waves (each with its colour dot and its own modules) and the
// preset chip ‹ name ›. OSC has one row per zone, its name vertical on the left (SOURCES, TIMBRE, ENV, LFO, FX, GLOBAL);
// a click on the name minimises the row to a bar. A module: dot and name, its type on the right (a list where the
// type is a choice), a screen, then its main knobs with the value in a box; "Mais" shows its other controls. On /
// off is a switch and a choice is a list, never a knob. MATRIX is every link as a table. ENV, LFO and Macros have the
// edit square, which shows the module's own links. Every change is one history step.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PatternEditor.h"
#include "samples.hpp"
#include "wcore.hpp"

namespace core::app {

class WCoreModular final : public juce::Component {
public:
  WCoreModular(PatternEditor::Host& host, std::string trackId);
  ~WCoreModular() override;
  const std::string& target() const { return trackId_; }
  void documentChanged();  // knobs and links follow undo/redo and other windows

  void paint(juce::Graphics& g) override;
  void resized() override;

  // The same operations the mouse does.
  void setParam(const std::string& param, double value);
  void setTail(const std::string& param, double value);  // a knob of the mood's Core FX tail: Wash ("amount"), Dry, delay, reverb
  void selectPrincipal(int layer);  // 0..3 (adds it when it does not exist yet)
  void showWaveMenu(int layer, juce::Component& at);  // right-click on a Wave: its colour, remove it
  juce::Colour layerColour(int layer) const;          // the Wave's own colour, or a shade of the track's
  void edit(const std::string& module);  // the edit square: show / hide its links ("" = none)
  void showTab(int tab);  // 0 OSC, 1 MATRIX
  void showMatrix(bool show);  // the MATRIX tab (or back to OSC)
  void foldZone(int zone, bool folded);  // minimise a row (0 SOURCES .. 5 GLOBAL) to a bar
  void showMore(const std::string& module, bool show);  // a module's other controls ("Mais")
  void addLink(const std::string& source);  // from lfo1.., env1.., macro1.., velocity, note, modwheel
  void setLink(const std::string& linkId, const doc::json& changes);
  void removeLink(const std::string& linkId);
  void setLfo(int lfo, const doc::json& changes);  // shape, mode, sync
  void applyPreset(const std::string& presetId);
  void chooseGrainSound(juce::Component* from);  // the Sample module's sound: a library sample, or the synth's own wave
  std::function<void()> onClose;
  std::function<const doc::SampleIndex*()> sampleLibrary;  // the scanned library, for the Sample module's menu
  std::function<void(const std::string& trackId, const std::string& pluginId)> onOpenPanel;  // Panels: another Core window
  std::function<void(const std::string& trackId)> onOpenModulation;  // Pista: the track's modulation (ducking)

  // The modules, in their zones (static: the window's fixed layout).
  struct ModuleDef {
    const char* id;       // "osc1", "filter1", "lfo2"...
    const char* name;     // what the header says
    const char* kicker;   // its type in words, on the right of its header ("" = none, or the type is a list)
    int zone;             // its row: 0 SOURCES, 1 TIMBRE, 2 ENV, 3 LFO, 4 FX (the tail), 5 GLOBAL
    const char* section;  // its light (doc::isWaveSection) or nullptr: always on
    const char* source;   // the modulation source it is ("lfo1", "env2", "macro"...) or nullptr
    std::vector<const char*> knobs;  // the engine's parameters (layer 1's names)
    int primary;                     // the first `primary` are the module's face; the rest show with "Mais"
  };
  static const std::vector<ModuleDef>& modules();

private:
  class Card;
  class Matrix;
  class Header;
  struct Content;
  void rebuild();               // the tab's cards (and the matrix) for the principal and the edit square
  std::string structureKey() const;  // what rebuild depends on in the document (links, LFO shapes, waves)
  const doc::Track* track() const;
  doc::json layerSynth() const;
  int layer() const { return layer_; }
  bool lfoUsed(int lfo) const;
  juce::Colour rowColour(int zone) const;  // the highlights of a row (the synth's blue-to-green scale)  // a link of this Wave starts at LFO lfo+1

  PatternEditor::Host& host_;
  std::string trackId_;
  int layer_ = 0;
  std::string editing_;           // the module whose edit square is on
  int tab_ = 0;                   // 0 OSC, 1 MATRIX
  std::set<int> foldedZones_;     // the rows minimised to a bar (this window)
  std::set<std::string> more_;    // the modules showing their other controls (this window)
  std::string builtFor_;
  bool dragging_ = false;
  std::unique_ptr<Header> header_;
  juce::Viewport view_;
  std::unique_ptr<Content> content_;
  friend class Card;
  friend class Matrix;
  friend class Header;
};

// The window around it: its own, resizable, one per track; the ✕ closes it.
class WCoreWindow final : public juce::DocumentWindow {
public:
  WCoreWindow(PatternEditor::Host& host, const std::string& trackId, const juce::String& trackName);
  void closeButtonPressed() override { if (onClose) onClose(); }
  WCoreModular& modular() { return *modular_; }
  std::function<void()> onClose;

private:
  WCoreModular* modular_ = nullptr;
};

}  // namespace core::app
