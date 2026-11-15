cmake_minimum_required(VERSION 3.20)
project(core_engine CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/W3 /utf-8 /fp:fast)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: one .exe, no VC++ redist
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Engine core: no audio device, no OS calls. Used by the app, the tests and the measurements.
add_library(core_engine_lib STATIC
  src/core/project.cpp src/core/tempo_map.cpp src/core/graph.cpp src/core/engine.cpp src/core/server.cpp src/core/render_job.cpp src/core/assets.cpp src/core/qualify.cpp
  src/dsp/instruments.cpp src/dsp/wavetables.cpp src/dsp/sampler.cpp src/dsp/sample_path.cpp src/dsp/effects.cpp src/dsp/devices.cpp src/dsp/registry.cpp src/dsp/loudness.cpp src/dsp/output_guard.cpp src/dsp/analyzer.cpp src/io/wav.cpp src/io/audio_file.cpp)
target_include_directories(core_engine_lib PUBLIC src third_party)

# Saturation 2's up-sampler in AVX (oversample_simd.hpp): only this unit gets AVX, and no FMA or
# contraction, so its sums are the scalar ones bit for bit. On by default for the Windows x64 build;
# anywhere else the engine takes the scalar path unless asked (-DCORE_DSP_AVX=ON on an x86-64 compiler).
set(_core_dsp_avx_default OFF)
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_core_dsp_avx_default ON)
endif()
option(CORE_DSP_AVX "Build the AVX kernel of the saturation 2 up-sampler" ${_core_dsp_avx_default})
if(CORE_DSP_AVX)
  target_sources(core_engine_lib PRIVATE src/dsp/saturation_avx.cpp)
  target_compile_definitions(core_engine_lib PRIVATE CORE_DSP_AVX=1)
  if(MSVC)
    set_source_files_properties(src/dsp/saturation_avx.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX;/fp:precise")
  else()
    set_source_files_properties(src/dsp/saturation_avx.cpp PROPERTIES COMPILE_OPTIONS "-mavx;-mno-fma;-ffp-contract=off")
  endif()
endif()

# miniaudio in its own unit (big header, compiled once).
add_library(miniaudio_impl STATIC src/io/miniaudio_impl.c)
target_include_directories(miniaudio_impl PUBLIC third_party)
if(UNIX AND NOT APPLE)
  target_link_libraries(miniaudio_impl PUBLIC pthread m dl)
endif()

if(MINGW)
  set(PLATFORM_LIBS ole32 winmm avrt)
endif()
add_executable(core-engine src/app/main.cpp src/app/measure.cpp src/io/device.cpp)
target_include_directories(core-engine PRIVATE src)
target_link_libraries(core-engine PRIVATE core_engine_lib miniaudio_impl ${PLATFORM_LIBS})
# The build's version and commit (core-engine --version, the hello, the log), when built from the root.
if(TARGET core_version)
  target_link_libraries(core-engine PRIVATE core_version)
  add_dependencies(core-engine core_version_header)
endif()
if(WIN32)
  target_link_libraries(core-engine PRIVATE dbghelp)  # crash dumps (serve --dumps)
endif()

# VST3 hosting, with JUCE when the root build has it: only to A/B Core's own sounds against a reference
# plugin (Serum 2) in the same engine. Core's sound stays its own. The JUCE code stays inside this library;
# core-engine sees only plugins/juce_host.hpp.
if(TARGET juce::juce_audio_processors)
  add_library(core_plugin_host STATIC src/plugins/juce_host.cpp)
  target_include_directories(core_plugin_host PUBLIC src third_party)
  target_link_libraries(core_plugin_host
    PRIVATE juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags
    PUBLIC core_engine_lib)
  target_compile_definitions(core_plugin_host PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
  target_link_libraries(core-engine PRIVATE core_plugin_host)
  target_compile_definitions(core-engine PRIVATE CORE_ENGINE_PLUGINS=1)
endif()
