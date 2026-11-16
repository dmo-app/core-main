# core-ai: Core's AI as a plugin, in its own process (Fase 4, 2026-11-16). Core.exe never links any of this: it starts
# core-ai.exe when the AI panel opens and talks to it through pipes (app/ai-link). The process owns the chat, the
# knowledge base, the memory of recipes (SQLite) and the chats; Core owns the catalog, the check and the Undo.
add_library(core_ai_service STATIC src/service.cpp)
target_include_directories(core_ai_service PUBLIC src)
target_link_libraries(core_ai_service PUBLIC core_ai core_ai_link)

if(CORE_BUILD_APP AND WIN32)
  # HTTPS to the model through JUCE's URL (juce_core only: no window, no audio).
  juce_add_console_app(core-ai PRODUCT_NAME "core-ai" VERSION ${CORE_VERSION})
  target_sources(core-ai PRIVATE src/main.cpp)
  target_compile_definitions(core-ai PRIVATE CORE_AI_JUCE=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
  target_link_libraries(core-ai PRIVATE core_ai_service juce::juce_core PUBLIC juce::juce_recommended_config_flags)
else()
  # Elsewhere (the cloud's tests): no network, only --fake-model and --evil.
  add_executable(core-ai src/main.cpp)
  target_link_libraries(core-ai PRIVATE core_ai_service)
endif()
set_target_properties(core-ai PROPERTIES OUTPUT_NAME core-ai)

# The "Core AI" package: everything that comes with the AI, and nothing of it in Core's package (Diogo, 2026-11-16).
#   build/package/Core AI/core-ai.exe, ai/core-ai-knowledge.sqlite, vst3/Core AI Gate.vst3
# Installed into %LOCALAPPDATA%\Programs\Core AI, where Core looks for it (or set CORE_AI_EXE).
set(CORE_AI_PACKAGE "${CMAKE_BINARY_DIR}/package/Core AI")
set(_ai_package_commands
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${CORE_AI_PACKAGE}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CORE_AI_PACKAGE}/ai"
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:core-ai> "${CORE_AI_PACKAGE}"
  COMMAND ${CMAKE_COMMAND} -E copy ${CORE_AI_KNOWLEDGE_DB} "${CORE_AI_PACKAGE}/ai/core-ai-knowledge.sqlite")
set(_ai_package_deps core-ai core_ai_knowledge)
if(TARGET CoreAIGate_VST3)
  list(APPEND _ai_package_commands
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:CoreAIGate_VST3>/../.." "${CORE_AI_PACKAGE}/vst3/Core AI Gate.vst3")
  list(APPEND _ai_package_deps CoreAIGate_VST3)
endif()
add_custom_target(core-ai-package ${_ai_package_commands} DEPENDS ${_ai_package_deps} COMMENT "The Core AI package" VERBATIM)
