# core-ai: Core's AI (2026-11-16): the chat with the model, its tools (Core's calls), the knowledge base, the memory of
# recipes (SQLite) and the cost of each message. Since Fase 4 it is linked ONLY into the AI process (core-ai,
# app/ai-process), never into Core.exe: Core reaches it through core_ai_link (pipes). No JUCE and no network here:
# the process sends the request bodies and hands back the answers.
add_library(core_ai STATIC src/ai_chat.cpp src/provider.cpp src/knowledge.cpp src/memory.cpp src/models.cpp src/stream.cpp src/kick_bass.cpp)
target_compile_features(core_ai PUBLIC cxx_std_20)
target_include_directories(core_ai PUBLIC src)
target_link_libraries(core_ai PUBLIC core_ai_link core_control PRIVATE core_sqlite)

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

# The cost and quality bench with the real API (tools/ai-bench): never run by ctest, it spends money.
add_executable(core-ai-bench ${CMAKE_SOURCE_DIR}/tools/ai-bench/ai_bench.cpp)
target_link_libraries(core-ai-bench PRIVATE core_ai)
