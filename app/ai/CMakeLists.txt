# core-ai: the AI inside Core (2026-11-16): the chat with Claude, its tools (Core's calls) and the cost of each message.
# No JUCE and no network: the app sends the request bodies and hands back the answers (desktop/AiPanel).
add_library(core_ai STATIC src/catalog.cpp src/ai_chat.cpp src/provider.cpp src/knowledge.cpp src/models.cpp)
target_compile_features(core_ai PUBLIC cxx_std_20)
target_include_directories(core_ai PUBLIC src)
target_link_libraries(core_ai PUBLIC core_control PRIVATE core_sqlite)

# The knowledge base (ai/knowledge/*.jsonl, from CORE-MANUAL) built into SQLite at build time:
# <build>/ai/core-ai-knowledge.sqlite; the app's build copies it beside Core.exe (ai/).
add_executable(core-ai-knowledge ${CMAKE_SOURCE_DIR}/tools/ai-knowledge/build_knowledge.cpp)
target_link_libraries(core-ai-knowledge PRIVATE core_ai)
set(CORE_AI_KNOWLEDGE_DB ${CMAKE_BINARY_DIR}/ai/core-ai-knowledge.sqlite CACHE INTERNAL "")
add_custom_command(OUTPUT ${CORE_AI_KNOWLEDGE_DB}
  COMMAND core-ai-knowledge ${CMAKE_SOURCE_DIR}/ai/knowledge/core-manual.jsonl ${CORE_AI_KNOWLEDGE_DB}
  DEPENDS core-ai-knowledge ${CMAKE_SOURCE_DIR}/ai/knowledge/core-manual.jsonl
  COMMENT "AI knowledge base (CORE-MANUAL)")
add_custom_target(core_ai_knowledge ALL DEPENDS ${CORE_AI_KNOWLEDGE_DB})
