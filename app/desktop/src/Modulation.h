// Modulation (UI02d), beside the SOUND card under the pattern editor (an audio track's: under the Mixer's strips):
// the track's modulators, in three tabs. A right-click on a SOUND bar adds one on that control; a row dragged onto a
// bar moves it there (Diogo 2026-11-07: "tem de estar tudo junto").
//  - LFO: a wave in time with the song (shape, rate in note lengths, depth, destination, phase).
//  - ENV: an envelope started by each note of the track, or of another track (attack, decay, trigger).
//  - STEP: a step sequence in time with the song, drawn with the mouse (step length, number of steps).
// A list of the tab's modulators on the left, a picture of the chosen one, then its controls and the range it
// sweeps. MACRO is shown, disabled (not in the engine yet). Every change goes through the domain
// (setModulator / setEnvelopeModulator / setStepModulator / removeModulator) as a history step; a drag is one gesture.
#pragma once
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "PatternEditor.h"

namespace core::app {

class ModulationPanel final : public juce::Component, private juce::Timer {
public:
  enum class Mode { Lfo, Env, Step };

  explicit ModulationPanel(PatternEditor::Host& host);
  ~ModulationPanel() override;

  void setTarget(const std::string& trackId);  // "" = the master (no modulation)
  void documentChanged();
  void addLfo();  // the same as a click on «+ Add LFO» (self-test too)
  void setMode(Mode mode);
  void addModulator();  // the current tab's «+ Add ...»
  // From the SOUND card: a new modulator of this kind on that control (right-click on a bar), or an existing one moved
  // onto it (a modulator dragged from the list onto a bar). Both are one Undo.
  void modulate(Mode mode, const std::string& param);
  void setDestination(const std::string& modulatorId, const std::string& param);
  static constexpr const char* kDragPrefix = "core-modulator:";  // drag description: the prefix, then the id
  static constexpr int kCompactHeight = 124;                      // beside or under the SOUND card

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;

private:
  struct Destination { std::string param; juce::String label, unit; double span; bool octaves; };
  std::vector<Destination> destinations() const;  // what the track's modulators may move
  const Destination* destinationOf(const std::string& param) const;
  void change(const juce::String& label, const std::function<void(nlohmann::json&)>& edit);  // the chosen modulator
  void updateControls();
  void drawStep(juce::Point<int> at);
  const nlohmann::json* chosen() const;
  juce::Rectangle<int> listArea() const;
  juce::Rectangle<int> chartArea() const;

  PatternEditor::Host& host_;
  Mode mode_ = Mode::Lfo;
  std::string trackId_, chosen_;
  std::vector<Destination> dests_;
  std::vector<std::string> sources_;  // the trigger combo's tracks ("" = own notes)
  nlohmann::json lfos_ = nlohmann::json::array();  // the current tab's modulators
  int total_ = 0;                                  // all of the track's modulators (the engine's limit is shared)
  bool drawing_ = false;
  // Live (30 Hz while shown): where the chosen modulator is now, a dot running over its picture.
  void timerCallback() override;
  void visibilityChanged() override;
  double liveWhere_ = -1, liveW_ = 0;  // liveWhere_ < 0: nothing to show (an envelope before its first note)
  int pressedRow_ = -1;  // the list row the mouse went down on (a drag from it moves that modulator onto a bar)
  juce::Label title_, empty_, name_, rangeLabel_, range_;
  juce::TextButton lfoTab_, envTab_, macroTab_, stepTab_, remove_;
  juce::ComboBox shape_, rate_, destination_, source_, stepCount_;
  theme::BarSlider depth_, phase_, attack_, decay_;  // drag to change, double-click to type
  juce::Label shapeLabel_, rateLabel_, depthLabel_, destinationLabel_, phaseLabel_, sourceLabel_, attackLabel_, decayLabel_, stepCountLabel_;
};

} // namespace core::app
