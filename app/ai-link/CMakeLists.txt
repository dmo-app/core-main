# core-ai-link: Core's side of its AI (Fase 4, 2026-11-16). The AI itself is a separate, optional process (core-ai,
# app/ai-process, its own package); Core keeps only what is the DAW's correctness: the catalog of actions, its closed
# scope (music creation only), the check of a proposal (all or nothing) and the wire to the process (pipes, no
# network). No JUCE, no SQLite, no model, no network. The Undo is the app's (one step per applied proposal).
add_library(core_ai_link STATIC src/catalog.cpp src/ai_scope.cpp src/ai_protocol.cpp src/ai_admit.cpp src/ai_link.cpp src/synth_roles.cpp)
target_compile_features(core_ai_link PUBLIC cxx_std_20)
target_include_directories(core_ai_link PUBLIC src)
target_link_libraries(core_ai_link PUBLIC core_control)
