// Core AI Gate: the JUCE shell around gate.hpp. The parameters are made from gate.hpp's table, in its order (their
// VST3 ParamIDs are their indexes), and read once per block into the Settings the gate plays.
#include <juce_audio_processors/juce_audio_processors.h>
#include "gate.hpp"

namespace {

using namespace core::aifx;

class GatePlugin final : public juce::AudioProcessor {
public:
  GatePlugin()
      : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    for (int p = 0; p < kParamCount; ++p) {
      const ParamInfo& i = params()[(size_t)p];
      juce::RangedAudioParameter* made = nullptr;
      if (p < kSteps) {
        made = new juce::AudioParameterChoice(juce::ParameterID{i.id, 1}, i.name, juce::StringArray{"Off", "Half", "On"}, (int)i.def);
      } else if (p == kRate) {
        juce::StringArray names;
        for (const auto& r : rateNames()) names.add(r);
        made = new juce::AudioParameterChoice(juce::ParameterID{i.id, 1}, i.name, names, (int)i.def);
      } else if (p == kAutoGain) {
        made = new juce::AudioParameterBool(juce::ParameterID{i.id, 1}, i.name, i.def > 0.5);
      } else {
        made = new juce::AudioParameterFloat(juce::ParameterID{i.id, 1}, i.name, juce::NormalisableRange<float>((float)i.min, (float)i.max), (float)i.def);
      }
      params_[(size_t)p] = made;
      addParameter(made);
    }
  }

  const juce::String getName() const override { return JucePlugin_Name; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override { return 0; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  bool hasEditor() const override { return true; }
  juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
  bool isBusesLayoutSupported(const BusesLayout& l) const override {
    const auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono()) && l.getMainInputChannelSet() == out;
  }
  void prepareToPlay(double sampleRate, int) override { gate_.prepare(sampleRate); }
  void releaseResources() override {}

  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    juce::ScopedNoDenormals noDenormals;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c) buffer.clear(c, 0, buffer.getNumSamples());
    Settings s;
    for (int i = 0; i < kSteps; ++i) s.steps[(size_t)i] = plain(kStep1 + i) >= 1.5 ? 2 : plain(kStep1 + i) >= 0.5 ? 1 : 0;
    s.rate = (int)plain(kRate);
    s.depth = plain(kDepth);
    s.length = plain(kLength);
    s.smoothMs = plain(kSmooth);
    s.duck = plain(kDuck);
    s.duckLength = plain(kDuckLength);
    s.duckCurve = plain(kDuckCurve);
    s.mix = plain(kMix);
    s.outputDb = plain(kOutput);
    s.autoGain = plain(kAutoGain) > 0.5;
    double ppq = 0, bpm = 120;
    bool playing = false;
    if (auto* head = getPlayHead())
      if (const auto pos = head->getPosition()) {
        playing = pos->getIsPlaying();
        if (const auto b = pos->getBpm()) bpm = *b;
        if (const auto q = pos->getPpqPosition()) ppq = *q;
        else playing = false;  // no position: the gate cannot be in time, so it stays open
      }
    gate_.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), s, ppq, bpm, playing);
  }

  // The state: every parameter's plain value by its id.
  void getStateInformation(juce::MemoryBlock& data) override {
    juce::XmlElement xml("CoreAIGate");
    xml.setAttribute("version", 1);
    for (int p = 0; p < kParamCount; ++p) xml.setAttribute(params()[(size_t)p].id, plain(p));
    copyXmlToBinary(xml, data);
  }
  void setStateInformation(const void* data, int size) override {
    const auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName("CoreAIGate")) return;
    for (int p = 0; p < kParamCount; ++p) {
      const ParamInfo& i = params()[(size_t)p];
      if (xml->hasAttribute(i.id))  // a parameter added later keeps its default in an older state
        params_[(size_t)p]->setValueNotifyingHost((float)toNormalized(p, xml->getDoubleAttribute(i.id, i.def)));
    }
  }

private:
  double plain(int p) const { return fromNormalized(p, params_[(size_t)p]->getValue()); }

  std::array<juce::RangedAudioParameter*, kParamCount> params_{};
  Gate gate_;
};

}  // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GatePlugin(); }
