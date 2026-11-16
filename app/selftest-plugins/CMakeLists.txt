# Small VST3 plugins made here, for Core.exe --self-test only (2026-11-16: Core's sound comes from plugins, so the
# self-test needs one it knows): an instrument (a sine per note) and an effect (gain). Built into the build folder;
# never copied into the system's VST3 folder, never shipped.
juce_add_plugin(CoreTestSynth
  PRODUCT_NAME "Core Test Synth"
  COMPANY_NAME "Core"
  VERSION 1.0.0
  PLUGIN_MANUFACTURER_CODE Core
  PLUGIN_CODE CTsy
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  FORMATS VST3
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_AUTO_MANIFEST TRUE)
target_sources(CoreTestSynth PRIVATE TestPlugins.cpp)
target_compile_definitions(CoreTestSynth PUBLIC CORE_TEST_SYNTH=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(CoreTestSynth PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)

juce_add_plugin(CoreTestGain
  PRODUCT_NAME "Core Test Gain"
  COMPANY_NAME "Core"
  VERSION 1.0.0
  PLUGIN_MANUFACTURER_CODE Core
  PLUGIN_CODE CTgn
  IS_SYNTH FALSE
  FORMATS VST3
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_AUTO_MANIFEST TRUE)
target_sources(CoreTestGain PRIVATE TestPlugins.cpp)
target_compile_definitions(CoreTestGain PUBLIC CORE_TEST_GAIN=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(CoreTestGain PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)
