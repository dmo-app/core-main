# core-engine-client: starts the engine process and speaks its protocol. No GUI, no DSP.
if(NOT WIN32)
  message(STATUS "core-engine-client: Windows only for now, skipped")
  return()
endif()
add_library(core_engine_client STATIC src/engine_client.cpp)
target_compile_features(core_engine_client PUBLIC cxx_std_20)
target_include_directories(core_engine_client PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/third_party)

