# core-previews: what each pattern of a track sounds like, rendered offline by the engine's own instruments and
# effects, so the arrangement can draw a real waveform inside every clip (Diogo, 2026-11-06: "nas pistas deveria
# se ver as waves dos samples e nao blocos"). No JUCE: the window caches and draws the result.
add_library(core_previews STATIC src/pattern_sound.cpp)
target_compile_features(core_previews PUBLIC cxx_std_20)
target_include_directories(core_previews PUBLIC src)
target_link_libraries(core_previews PUBLIC core_domain core_engine_lib)

