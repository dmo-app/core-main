// Core plays plugins (Diogo 2026-11-16): its own effects (EQ, filter, compressor, limiter, utility, Core FX, Core EQ and
// the Core Devices) left the engine. A chain holds plugin effects only; no built-in effect is registered.
#include "effects.hpp"
#include "registry.hpp"

namespace core {

void registerNativeEffects(NativeRegistry&) {}

std::unique_ptr<Effect> makeEffect(const std::string&, double, double, int) { return nullptr; }

}  // namespace core
