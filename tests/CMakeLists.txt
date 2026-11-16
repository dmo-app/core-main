# Core's tests (2026-11-16). Two levels:
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#   label "merge":   fast, every build and before every merge (ctest -L merge)
#   label "desktop": opens Core's window (the app's --self-test); only where the app is built
# Signals are generated in the tests; no audio fixtures and no bit-exact goldens tied to one compiler.


add_executable(engine_tests engine_tests.cpp ${CMAKE_SOURCE_DIR}/engine/src/app/measure.cpp ${CMAKE_SOURCE_DIR}/engine/src/io/device.cpp)
target_include_directories(engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine_tests PRIVATE core_engine_lib miniaudio_impl)
if(WIN32)
  target_link_libraries(engine_tests PRIVATE dbghelp)
endif()
add_test(NAME engine_tests COMMAND engine_tests)

add_executable(domain_tests domain_tests.cpp)
target_include_directories(domain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(domain_tests PRIVATE core_domain core_engine_lib)
target_compile_definitions(domain_tests PRIVATE CORE_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/templates")
add_test(NAME domain_tests COMMAND domain_tests)

# The AI inside Core: the song's calls, the chat (canned answers, no network), the knowledge base, the providers.
add_executable(ai_tests ai_tests.cpp)
target_include_directories(ai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ai_tests PRIVATE core_ai)
target_compile_definitions(ai_tests PRIVATE CORE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ai_tests COMMAND ai_tests)

# Rede de seguranca da AI (plano de testes T05-T08): modelo falso, um contrato por acao, undo unico, ambito.
add_executable(ai_contract_tests ai_contract_tests.cpp)
target_include_directories(ai_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/engine/src)
target_link_libraries(ai_contract_tests PRIVATE core_ai core_domain core_engine_lib)
target_compile_definitions(ai_contract_tests PRIVATE CORE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ai_contract_tests COMMAND ai_contract_tests)

# Core AI Gate (app/aifx): the gate's sound, its role, toolGate and the AI's "gate" call.
add_executable(aifx_tests aifx_tests.cpp)
target_include_directories(aifx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aifx_tests PRIVATE core_ai core_domain core_engine_lib)
add_test(NAME aifx_tests COMMAND aifx_tests)

# The role map (Settings > Roles) and the tools that play through it (SUCK, PUM, build-up effects).
add_executable(role_tests role_tests.cpp)
target_include_directories(role_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(role_tests PRIVATE core_domain core_engine_lib)
add_test(NAME role_tests COMMAND role_tests)

# MIDI files in and out (export with the rolls expanded as they play; import into tracks and patterns).
add_executable(midi_tests midi_tests.cpp)
target_include_directories(midi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(midi_tests PRIVATE core_domain core_engine_lib)
add_test(NAME midi_tests COMMAND midi_tests)

# The medium and small bugs fixed from the reviewed sweep (2026-11-16): each failed before its fix.
add_executable(bug_tests bug_tests.cpp)
target_include_directories(bug_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bug_tests PRIVATE core_domain core_engine_lib)
add_test(NAME bug_tests COMMAND bug_tests)

# The export for the Bitwig: the .mid and the .vstpreset of each plugin, and which goes on which track.
add_executable(bitwig_tests bitwig_tests.cpp)
target_include_directories(bitwig_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bitwig_tests PRIVATE core_domain core_engine_lib)
add_test(NAME bitwig_tests COMMAND bitwig_tests)

# The bugs of the external audit (2026-11-16, RELATORIO-BUGS-MELHORIAS.md): each failed before its fix.
add_executable(audit_tests audit_tests.cpp)
target_include_directories(audit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/engine/src)
target_link_libraries(audit_tests PRIVATE core_domain core_engine_lib)
add_test(NAME audit_tests COMMAND audit_tests)

# The second external audit (2026-11-16, code sweep): engine-client lines, sync retry, export disk space, autosave.
add_executable(audit2_tests audit2_tests.cpp)
target_include_directories(audit2_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/engine/src ${CMAKE_SOURCE_DIR}/app/engine-client/src)
target_link_libraries(audit2_tests PRIVATE core_domain core_engine_lib)
add_test(NAME audit2_tests COMMAND audit2_tests)

# The deep bug hunt (2026-11-16, RELATORIO-CAÇA-PROFUNDA.md, P01-P06): each case of the report.
add_executable(hunt_tests hunt_tests.cpp)
target_include_directories(hunt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/engine/src ${CMAKE_SOURCE_DIR}/app/engine-client/src)
target_link_libraries(hunt_tests PRIVATE core_domain core_engine_lib core_previews core_sampling)
target_compile_definitions(hunt_tests PRIVATE CORE_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/templates" CORE_LANG_DIR="${CMAKE_SOURCE_DIR}/lang")
add_test(NAME hunt_tests COMMAND hunt_tests)

# The arrangement's time selection (2026-11-16): copy, clear, paste and duplicate a piece of time on several tracks.
add_executable(time_range_tests time_range_tests.cpp)
target_include_directories(time_range_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(time_range_tests PRIVATE core_domain core_engine_lib)
add_test(NAME time_range_tests COMMAND time_range_tests)
add_executable(pattern_rack_tests pattern_rack_tests.cpp)
target_include_directories(pattern_rack_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pattern_rack_tests PRIVATE core_domain core_engine_lib)
add_test(NAME pattern_rack_tests COMMAND pattern_rack_tests)

add_executable(song_notes_tests song_notes_tests.cpp)
target_include_directories(song_notes_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(song_notes_tests PRIVATE core_domain core_engine_lib)
add_test(NAME song_notes_tests COMMAND song_notes_tests)

# Ronda 3 do audit (R3-A a R3-G).
add_executable(round3_tests round3_tests.cpp)
target_include_directories(round3_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(round3_tests PRIVATE core_domain core_engine_lib)
target_compile_definitions(round3_tests PRIVATE CORE_LANG_DIR="${CMAKE_SOURCE_DIR}/lang")
add_test(NAME round3_tests COMMAND round3_tests)

# The command line: what the app and the scripts call.
add_test(NAME cli_version COMMAND core-engine --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "core-engine ${CORE_VERSION}")
add_test(NAME cli_demo_render COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:core-engine> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                                      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_demo_render.cmake)
add_test(NAME cli_measure COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:core-engine> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_measure.cmake)

add_test(NAME cli_unicode COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:core-engine> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_unicode.cmake)

# Quality of life from other DAWs (2026-11-16): sections as markers, Double and row shift in Patterns, nudge, Freeze.
add_executable(quick_tools_tests quick_tools_tests.cpp)
target_include_directories(quick_tools_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/engine/src)
target_link_libraries(quick_tools_tests PRIVATE core_domain core_engine_lib)
add_test(NAME quick_tools_tests COMMAND quick_tools_tests)

# O ci/gate.ps1 sem PowerShell: .bat com nome por corrida, nenhuma variavel com o nome de um parametro, JUCE_BUILDNUMBER.
add_test(NAME gate_static COMMAND ${CMAKE_COMMAND} -DGATE=${CMAKE_SOURCE_DIR}/ci/gate.ps1 -P ${CMAKE_CURRENT_SOURCE_DIR}/gate_static.cmake)

set_tests_properties(engine_tests domain_tests ai_tests ai_contract_tests aifx_tests role_tests midi_tests bug_tests audit_tests audit2_tests hunt_tests round3_tests bitwig_tests time_range_tests pattern_rack_tests song_notes_tests quick_tools_tests cli_version cli_demo_render cli_measure cli_unicode gate_static
                     PROPERTIES LABELS merge TIMEOUT 300)
# Labels por area (2026-11-16, plano de testes T03), alem do nivel. ci/testes.txt e a lista que o gate confere.
#   ai      o que a AI chama (tools, catalogo, propostas)
#   ui      a app com janela (eventos de rato e teclado)
#   plugins plugins reais (Vital, sfizz); fora do gate, a mao (T19). Vazio ate la.
set_tests_properties(ai_tests ai_contract_tests aifx_tests role_tests round3_tests PROPERTIES LABELS "merge;ai")

# The app's own end-to-end test (play, undo, save/reopen, export... on the null device, with its test plugins).
# It opens Core's window for ~20 s, so it has its own label.
if(TARGET CoreApp)
  add_test(NAME desktop_self_test COMMAND CoreApp --self-test ${CMAKE_CURRENT_BINARY_DIR}/self-test.png)
  set_tests_properties(desktop_self_test PROPERTIES LABELS "desktop;ui" TIMEOUT 600)
endif()

