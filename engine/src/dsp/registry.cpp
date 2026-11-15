#include "registry.hpp"

namespace core {

const NativePluginDesc* NativeRegistry::find(NativeKind kind, const std::string& id) const {
  for (const auto& d : items_)
    if (d.kind == kind && d.id == id) return &d;
  return nullptr;
}

std::vector<std::string> NativeRegistry::names(NativeKind kind) const {
  std::vector<std::string> out;
  for (const auto& d : items_)
    if (d.kind == kind) out.push_back(d.id);
  return out;
}

NativeRegistry& nativePlugins() {
  // Built the first time it is asked for, from the DSP translation units. A magic static: thread-safe, and
  // there is no static-initialisation order to depend on (a Graph built in a static initializer still works).
  static NativeRegistry reg = [] {
    NativeRegistry r;
    registerNativeInstruments(r);
    registerNativeEffects(r);
    return r;
  }();
  return reg;
}

namespace {
const std::vector<ParamSpec>& emptyParams() { static const std::vector<ParamSpec> none; return none; }
} // namespace

const std::vector<ParamSpec>& nativeInstrumentParams(const std::string& id) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Instrument, id);
  return d ? d->params : emptyParams();
}
const std::vector<ParamSpec>& nativeEffectParams(const std::string& id) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Effect, id);
  return d ? d->params : emptyParams();
}
int nativeInstrumentVersion(const std::string& id) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Instrument, id);
  return d ? d->version : 1;
}
int nativeEffectVersion(const std::string& id) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Effect, id);
  return d ? d->version : 1;
}
bool nativeIsInstrument(const std::string& id) { return nativePlugins().find(NativeKind::Instrument, id) != nullptr; }
bool nativeInstrumentHasNoParams(const std::string& id) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Instrument, id);
  return d && d->noParams;
}
std::vector<std::string> nativeInstrumentNames() { return nativePlugins().names(NativeKind::Instrument); }
std::vector<std::string> nativeEffectNames() { return nativePlugins().names(NativeKind::Effect); }

} // namespace core
