// A built-in instrument's page (kick, Sampler, hats, drums, snare): its SOUND bars and its MODULATION together, so a
// sound is made and moved in one place (Diogo 2026-11-07: "isto nao pode estar numa pagina separada da criaçao",
// "como é um plugin grande façam abrir numa pagina separada", "gosto do minimalismo do core, mas tem de estar tudo
// junto"). The filter's curve moves with the modulation while the song plays ("quero ver o efeito dos filtros").
// Opened from the card under the pattern editor («Open», or a right-click on a bar). Escape or ✕ closes it.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Modulation.h"
#include "PatternEditor.h"
#include "SoundStrip.h"

namespace core::app {

class SoundPage final : public juce::Component {
public:
  explicit SoundPage(PatternEditor::Host& host);
  ~SoundPage() override;
  void setTarget(const std::string& trackId);
  const std::string& target() const { return trackId_; }
  bool shows() const { return sound_.shows(); }
  void documentChanged();
  void modulate(const std::string& param, int kind);  // the same as the bar's right-click menu
  ModulationPanel& modulation() { return modulation_; }
  std::function<void()> onClose;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  PatternEditor::Host& host_;
  std::string trackId_;
  juce::Label title_;
  juce::TextButton close_;
  SoundStrip sound_;
  ModulationPanel modulation_;
  class FilterView;  // the filter's curve, moving with the modulation (kick, Sampler: instruments with a Cutoff)
  std::unique_ptr<FilterView> filter_;
};

}  // namespace core::app
