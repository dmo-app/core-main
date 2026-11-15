// The registry of the built-in ("core") plugins: every internal instrument and effect describes itself
// here once — its id, kind, sound version, parameter catalogue and how to build it — so there is a single
// source of truth instead of the id, the parameters, the version and the factory being spread over
// project.cpp, instruments.cpp and effects.cpp. Adding a native plugin is one entry next to its class.
//
// Layering: this lives in the DSP layer and depends only on the DSP headers, so the core (project.cpp,
// graph.cpp) can query it without the DSP ever depending back on the core. It is consulted only on the
// control thread (Graph build, validation); the audio thread keeps calling the Instrument/Effect objects
// exactly as before, so nothing in the real-time path changes.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "param_spec.hpp"
#include "instruments.hpp"
#include "effects.hpp"

namespace core {

enum class NativeKind { Instrument, Effect };

struct NativePluginDesc {
  std::string id;                 // the document name and the registry key ("kick", "eq")
  NativeKind kind;
  int version = 1;                // the current sound/DSP version (a song keeps the version it was saved with)
  bool noParams = false;          // plugin / audio / bus: a track with no built-in instrument parameters
  std::vector<ParamSpec> params;  // ordered; the position is the runtime params[] slot
  // How to build it. makeInstrument is null for the sampler (the graph builds it, it needs the zones);
  // makeEffect takes (sampleRate, bpm, version). Only the matching one is set for a kind.
  std::function<std::unique_ptr<Instrument>(double sampleRate)> makeInstrument;
  std::function<std::unique_ptr<Effect>(double sampleRate, double bpm, int version)> makeEffect;
};

class NativeRegistry {
public:
  void add(NativePluginDesc desc) { items_.push_back(std::move(desc)); }
  const NativePluginDesc* find(NativeKind kind, const std::string& id) const;
  std::vector<std::string> names(NativeKind kind) const;  // in registration order
private:
  std::vector<NativePluginDesc> items_;  // built once, never modified after: find() pointers stay valid
};

// The one registry, populated lazily on first use from the DSP translation units (thread-safe magic
// static; no static-initialisation order to get wrong).
NativeRegistry& nativePlugins();
void registerNativeInstruments(NativeRegistry&);  // defined in instruments.cpp
void registerNativeEffects(NativeRegistry&);      // defined in effects.cpp

// Catalogue queries. project.cpp's public accessors (instrumentParams, effectNames, ...) delegate here,
// so the whole codebase keeps one source of truth.
const std::vector<ParamSpec>& nativeInstrumentParams(const std::string& id);  // empty: not a known instrument
const std::vector<ParamSpec>& nativeEffectParams(const std::string& id);      // empty: not a known effect
int nativeInstrumentVersion(const std::string& id);                           // 1 when unknown
int nativeEffectVersion(const std::string& id);                               // 1 when unknown
bool nativeIsInstrument(const std::string& id);
bool nativeInstrumentHasNoParams(const std::string& id);
std::vector<std::string> nativeInstrumentNames();
std::vector<std::string> nativeEffectNames();

} // namespace core
