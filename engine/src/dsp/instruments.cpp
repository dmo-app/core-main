// Core plays plugins (Diogo 2026-11-16): its own instruments (kick, W-Core, hats, snare, drums, sampler, Core Glitch)
// left the engine. What is registered here are the track kinds with no built-in sound: a plugin (the graph plays the
// plugin; this silent one stands in while it is missing), an audio track, a bus, and a track with notes and no
// instrument yet ("none", also an old song's Core instrument, kept in the song, not played).
#include "instruments.hpp"
#include "registry.hpp"

namespace core {

namespace {

class Silent final : public Instrument {
public:
  using Instrument::Instrument;
  const char* name() const override { return "plugin"; }
  void noteOn(int, double) override {}
  void noteOff(int) override {}
  void allNotesOff() override {}
  void render(float*, int) override {}
  void adopt(const Instrument&) override {}
};

}  // namespace

void registerNativeInstruments(NativeRegistry& reg) {
  // A placeholder spec keeps them "known" to validation; the graph gives them a silent instrument.
  const std::vector<ParamSpec> none{{"_", 0, 0, 0, ""}};
  const std::function<std::unique_ptr<Instrument>(double)> silent = [](double sr) { return std::make_unique<Silent>(sr); };
  for (const char* id : {"plugin", "audio", "bus", "none"}) reg.add({id, NativeKind::Instrument, 1, true, none, silent, nullptr});
}

std::unique_ptr<Instrument> makeInstrument(const std::string& name, double sampleRate) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Instrument, name);
  if (d && d->makeInstrument) return d->makeInstrument(sampleRate);
  return nullptr;
}

}  // namespace core
