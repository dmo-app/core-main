// One entry of a built-in ("core") plugin's parameter catalogue: its name, default and range, and the
// unit it is shown in. Shared by validation (core/project.cpp), the DSP and the app's catalogue.
// Defined here, in the DSP layer, so a plugin's parameters live with the plugin (see dsp/registry.hpp);
// core/project.hpp re-exposes it as core::ParamSpec for the rest of the code.
#pragma once

namespace core {

struct ParamSpec { const char* name; double def, min, max; const char* unit; };

} // namespace core
