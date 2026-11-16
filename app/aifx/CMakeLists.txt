# Core's own "AI FX" plugins (Diogo, 2026-11-16): small VST3 effects made here, driven by the AI through the role
# map. The sound of each is a std-only header in src/ (tested in tests/aifx_tests.cpp); the plugin is only the
# JUCE shell around it. Built into the build folder; the installer copies it into the VST3 folder.
juce_add_plugin(CoreAIGate
  PRODUCT_NAME "Core AI Gate"
  COMPANY_NAME "Core"
  VERSION 1.0.0
  PLUGIN_MANUFACTURER_CODE Core
  PLUGIN_CODE CAgt
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  FORMATS VST3
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_CATEGORIES Fx Dynamics
  VST3_AUTO_MANIFEST TRUE)
target_sources(CoreAIGate PRIVATE src/GatePlugin.cpp)
target_include_directories(CoreAIGate PRIVATE src)
# The ParamIDs are the parameters' indexes (gate.hpp keeps the order), so the role map knows them without the plugin.
target_compile_definitions(CoreAIGate PUBLIC JUCE_FORCE_USE_LEGACY_PARAM_IDS=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
                           JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(CoreAIGate PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)
