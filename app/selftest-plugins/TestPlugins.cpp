// The test plugins (one file, built once per plugin): CORE_TEST_SYNTH (a sine per note), CORE_TEST_GAIN or
// CORE_TEST_DELAY (a delay that reports it as latency), CORE_TEST_THROW (a gain that throws after 0.1 s, audit 6.4). All have one parameter, "gain" (0..1, 0.5), saved in their state, and the generic editor.
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {

class TestPlugin : public juce::AudioProcessor {
public:
  explicit TestPlugin(const BusesProperties& buses) : AudioProcessor(buses) {
    addParameter(gain_ = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 1.0f, 0.5f));
  }
  const juce::String getName() const override { return JucePlugin_Name; }
  double getTailLengthSeconds() const override { return 0; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  bool hasEditor() const override { return true; }
  juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
  void releaseResources() override {}
  void getStateInformation(juce::MemoryBlock& data) override { juce::MemoryOutputStream(data, false).writeFloat(gain_->get()); }
  void setStateInformation(const void* data, int size) override {
    if (size >= 4) gain_->setValueNotifyingHost(gain_->convertTo0to1(juce::MemoryInputStream(data, (size_t)size, false).readFloat()));
  }
protected:
  juce::AudioParameterFloat* gain_ = nullptr;
};

#if CORE_TEST_SYNTH
// A sine per note: frequency from the pitch, level velocity x gain, 2 ms attack, 20 ms release.
class Synth final : public TestPlugin {
public:
  Synth() : TestPlugin(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  bool isBusesLayoutSupported(const BusesLayout& l) const override { return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo(); }
  void prepareToPlay(double sampleRate, int) override { rate_ = sampleRate; for (auto& v : voices_) v = {}; }
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
    buffer.clear();
    int pos = 0;
    auto render = [&](int to) {
      for (auto& v : voices_) {
        if (v.level <= 0 && !v.on) continue;
        const double step = 2 * juce::MathConstants<double>::pi * 440.0 * std::pow(2.0, (v.pitch - 69) / 12.0) / rate_;
        for (int i = pos; i < to; ++i) {
          const double target = v.on ? 1.0 : 0.0, rate = v.on ? 1.0 / (0.002 * rate_) : 1.0 / (0.02 * rate_);
          v.level = target > v.level ? std::min(target, v.level + rate) : std::max(target, v.level - rate);
          const float s = (float)(std::sin(v.phase) * v.level * v.velocity * gain_->get());
          v.phase += step;
          buffer.addSample(0, i, s);
          buffer.addSample(1, i, s);
        }
      }
      pos = to;
    };
    for (const auto m : midi) {
      render(m.samplePosition);
      const auto msg = m.getMessage();
      if (msg.isNoteOn()) {
        Voice* free = &voices_[0];
        for (auto& v : voices_) if (!v.on && v.level <= 0) { free = &v; break; }
        *free = Voice{msg.getNoteNumber(), msg.getFloatVelocity(), 0, 0, true};
      } else if (msg.isNoteOff()) {
        for (auto& v : voices_) if (v.on && v.pitch == msg.getNoteNumber()) v.on = false;
      } else if (msg.isAllNotesOff()) {
        for (auto& v : voices_) v.on = false;
      }
    }
    render(buffer.getNumSamples());
  }
private:
  struct Voice { int pitch = 0; float velocity = 0; double phase = 0, level = 0; bool on = false; };
  Voice voices_[16];
  double rate_ = 48000;
};
juce::AudioProcessor* make() { return new Synth(); }
#endif

#if CORE_TEST_GAIN
// Multiplies by gain (0.5 when new).
class Gain final : public TestPlugin {
public:
  Gain() : TestPlugin(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isBusesLayoutSupported(const BusesLayout& l) const override {
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
  }
  void prepareToPlay(double, int) override {}
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain(gain_->get()); }
};
juce::AudioProcessor* make() { return new Gain(); }
#endif

#if CORE_TEST_DELAY
// Delays its input by kLatency samples and says so (setLatencySamples), as a lookahead limiter would.
class Delay final : public TestPlugin {
public:
  static constexpr int kLatency = 480;
  Delay() : TestPlugin(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    setLatencySamples(kLatency);
  }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isBusesLayoutSupported(const BusesLayout& l) const override {
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
  }
  void prepareToPlay(double, int) override { for (auto& c : line_) c.fill(0.0f); at_ = 0; }
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
      for (int c = 0; c < 2; ++c) std::swap(line_[(size_t)c][(size_t)at_], *buffer.getWritePointer(c, i));
      at_ = (at_ + 1) % kLatency;
    }
  }
private:
  std::array<std::array<float, kLatency>, 2> line_{};
  int at_ = 0;
};
juce::AudioProcessor* make() { return new Delay(); }
#endif

#if CORE_TEST_THROW
// Passes its input through for 0.1 s, then throws in every processBlock (audit 6.4, M3b): the engine must
// silence its track and play the others on.
class Throw final : public TestPlugin {
public:
  Throw() : TestPlugin(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isBusesLayoutSupported(const BusesLayout& l) const override {
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
  }
  void prepareToPlay(double sampleRate, int) override { limit_ = (int64_t)(0.1 * sampleRate); played_ = 0; }
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    if (played_ >= limit_) throw std::runtime_error("Core Test Throw: processBlock failed on purpose");
    played_ += buffer.getNumSamples();
  }
private:
  int64_t limit_ = 4800, played_ = 0;
};
juce::AudioProcessor* make() { return new Throw(); }
#endif

}  // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return make(); }
