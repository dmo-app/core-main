# core-domain: the app document, validation and projection. No GUI, no audio, no engine code.
add_library(core_domain STATIC src/document.cpp src/history.cpp src/storage.cpp src/edit.cpp src/assets.cpp src/sha256.cpp src/templates.cpp src/autosave.cpp src/samples.cpp src/sample_analysis.cpp src/fx_racks.cpp src/randomize.cpp src/groove.cpp src/training_clips.cpp src/rcore.cpp src/fcore.cpp src/sound_layers.cpp src/wcore.cpp src/score.cpp src/stutter.cpp src/attach.cpp src/pattern_tools.cpp src/pcore.cpp src/pattern_rack.cpp src/bounce.cpp src/core_plugins.cpp src/corefx_legacy.cpp src/glitch.cpp src/follow.cpp src/template_sheet.cpp src/plugin_refs.cpp)
target_compile_features(core_domain PUBLIC cxx_std_20)
# The help window of every plugin (its words; the window is app/desktop PluginHelp): docs/contracts/core-plugin-help.md.
target_sources(core_domain PRIVATE src/plugin_help.cpp)
# Core in another language (Settings > General > Language): docs/contracts/core-language.md.
target_sources(core_domain PRIVATE src/translation.cpp)
target_include_directories(core_domain PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/third_party)

# core-clips: training clips (20-30 s excerpts and their facts) from the style templates, rendered by core-engine.
add_executable(core-clips tools/core_clips.cpp)
target_link_libraries(core-clips PRIVATE core_domain)
