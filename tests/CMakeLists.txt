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

# The command line: what the app and the scripts call.
add_test(NAME cli_version COMMAND core-engine --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "core-engine ${CORE_VERSION}")
add_test(NAME cli_demo_render COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:core-engine> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                                      -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_demo_render.cmake)
add_test(NAME cli_measure COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:core-engine> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_measure.cmake)

set_tests_properties(engine_tests domain_tests cli_version cli_demo_render cli_measure PROPERTIES LABELS merge TIMEOUT 300)

# The app's own end-to-end test (play, undo, save/reopen, export... on the null device, with its test plugins).
# It opens Core's window for ~20 s, so it has its own label.
if(TARGET CoreApp)
  add_test(NAME desktop_self_test COMMAND CoreApp --self-test ${CMAKE_CURRENT_BINARY_DIR}/self-test.png)
  set_tests_properties(desktop_self_test PROPERTIES LABELS desktop TIMEOUT 600)
endif()
