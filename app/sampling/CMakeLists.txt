# core-sampling: making a sample from audio (Save as sample, Render to sample), without JUCE.
add_library(core_sampling STATIC src/sampling.cpp)
target_compile_features(core_sampling PUBLIC cxx_std_20)
target_include_directories(core_sampling PUBLIC src)
target_include_directories(core_sampling PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)  # dsp/clip_grains.hpp: the engine's clip grains
target_link_libraries(core_sampling PUBLIC core_domain)

