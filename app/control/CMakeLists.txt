# core-control: Core's functions as AICORE sees them (signatures, the song in a few lines), the proposals of calls
# AICORE writes and how Core checks and applies them, the file bridge between the two, and the «+ Effect» menu's
# plugin grouping. No JUCE, no engine code, no model: the app commits an applied proposal as one undo step.
add_library(core_control STATIC src/calls.cpp src/bridge.cpp)
target_compile_features(core_control PUBLIC cxx_std_20)
target_include_directories(core_control PUBLIC src)
target_link_libraries(core_control PUBLIC core_domain)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(core_control PUBLIC Threads::Threads)
endif()

