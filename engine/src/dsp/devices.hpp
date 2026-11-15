// Core Devices: built-in effects that each do one musical thing with a few knobs (GROWL, METAL, PUNCH, ZAP).
// They are ordinary built-in effects for the graph: registerNativeDevices adds each one (its knobs and its
// factory) to the native registry, right after the classic effects.
#pragma once
#include <memory>
#include <string>
#include "effects.hpp"
#include "param_spec.hpp"
#include <vector>

namespace core {
class NativeRegistry;
void registerNativeDevices(NativeRegistry& reg);
std::unique_ptr<Effect> makeDevice(const std::string& name, double sampleRate, double bpm, int version);
// CORE FX's building blocks ("space", "echo", "spiral", "flux"): their knobs, in params[] order. Empty otherwise.
const std::vector<ParamSpec>& coreFxBlockParams(const std::string& name);
}
