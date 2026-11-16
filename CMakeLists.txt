# Core: the native app (domain, engine client, JUCE window) and the audio engine.
# cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.22)
# The version of Core. Raise it for every build given to others or used for a sweep/audit report;
# the git commit is added by itself (Help > About, the window title, the logs, core-engine --version).
project(core VERSION 0.7.0 LANGUAGES CXX C)
set(CORE_VERSION ${PROJECT_VERSION})

# core_version.h (CORE_VERSION, CORE_GIT_HASH, CORE_BUILD_ID), refreshed at every build.
set(CORE_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/core_version.h)
add_custom_target(core_version_header
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUT=${CORE_VERSION_HEADER} -DVERSION=${CORE_VERSION}
          -P ${CMAKE_SOURCE_DIR}/cmake/core_version.cmake
  BYPRODUCTS ${CORE_VERSION_HEADER}
  COMMENT "Core version")
add_library(core_version INTERFACE)
target_include_directories(core_version INTERFACE ${CMAKE_BINARY_DIR}/generated)
# (each target that includes it also adds add_dependencies(<target> core_version_header))
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  # Same runtime for every target: the domain tests link the engine library.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)
endif()

option(CORE_BUILD_APP "Build the JUCE desktop app (downloads JUCE 9.0.5 unless FETCHCONTENT_SOURCE_DIR_JUCE is set)" ON)
if(CORE_BUILD_APP AND WIN32)
  # JUCE is pinned by version and hash. To build from a local copy instead of downloading it:
  #   -DFETCHCONTENT_SOURCE_DIR_JUCE=D:/Core-deps/JUCE-9.0.5
  include(FetchContent)
  FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.5.tar.gz  # commit d60b7a78030661574a615b586d9325ba7a7fa7dd
    URL_HASH SHA256=d92207f51ba728d3d195b6d5e93ecd049717a915fc250ff6844c32ea7272315a)
  FetchContent_MakeAvailable(JUCE)
endif()

add_subdirectory(engine)
add_subdirectory(app/domain)
add_subdirectory(app/engine-client)
add_subdirectory(app/control)  # Core's functions and the bridge to AICORE (no JUCE)
add_subdirectory(app/sampling) # making samples from audio (no JUCE)
add_subdirectory(app/previews) # what each pattern sounds like, for the clips' waveforms (no JUCE)
if(CORE_BUILD_APP AND WIN32)
  add_subdirectory(app/selftest-plugins)  # a test synth and gain VST3 for Core.exe --self-test (never shipped)
  add_subdirectory(app/desktop)
  add_subdirectory(app/installer)  # Core Setup.exe (M6)
endif()

# Tests (ctest -L merge before every merge; see tests/CMakeLists.txt and ci/).
option(CORE_BUILD_TESTS "Build Core's tests" ON)
if(CORE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
