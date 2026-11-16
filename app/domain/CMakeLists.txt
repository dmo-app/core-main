# core-domain: the app document, validation and projection. No GUI, no audio, no engine code.
# Core's own sound left on 2026-11-16 (Diogo: the sound comes from plugins): no W-Core, F-Core, R-Core, Style FX racks,
# Core FX conversion, Core Glitch, sound layers, Core plugin catalogue or help, training clips or attachments.
add_library(core_domain STATIC src/document.cpp src/history.cpp src/storage.cpp src/edit.cpp src/assets.cpp src/sha256.cpp src/templates.cpp src/autosave.cpp src/samples.cpp src/sample_analysis.cpp src/randomize.cpp src/groove.cpp src/score.cpp src/stutter.cpp src/pattern_tools.cpp src/pcore.cpp src/pattern_rack.cpp src/bounce.cpp src/follow.cpp src/template_sheet.cpp src/plugin_refs.cpp src/roles.cpp src/role_tools.cpp src/core_plugins.cpp src/midi.cpp src/bitwig.cpp)
target_compile_features(core_domain PUBLIC cxx_std_20)
# Core in another language (Settings > General > Language): docs/contracts/core-language.md.
target_sources(core_domain PRIVATE src/translation.cpp)
target_include_directories(core_domain PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/third_party)
