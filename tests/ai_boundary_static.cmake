# A fronteira Core <-> AI, verificada nas fontes (Fase 4, 2026-11-16; Diogo: a AI e um plugin, e tudo o que vem com
# ela e do plugin, a memoria e a base de dados incluidas). Corre na cloud sem compilar a app:
#   1. o Core.exe (app/desktop) nao linka core_ai nem core_sqlite, e nao inclui nenhum cabecalho da AI;
#   2. o lado do Core (app/ai-link) nao tem rede, SQLite, nem modelo;
#   3. o motor (engine/) nao conhece a AI (zero codigo de AI no caminho de audio);
#   4. o painel da AI so fala por pipes: sem pedidos HTTP, sockets ou WinHTTP;
#   5. o pacote do Core (stage do instalador, a pasta da build, o core-package) nao leva nada da AI;
#   6. a AI (app/ai, SQLite, o processo, o gate) so se compila com CORE_BUILD_AI: o Core compila sem ela.
#   cmake -DSOURCE=<repo> -P tests/ai_boundary_static.cmake
if(NOT DEFINED SOURCE)
  message(FATAL_ERROR "ai_boundary_static: falta -DSOURCE")
endif()
set(problems "")

function(read_code out file)
  # O texto sem comentarios // (o que interessa e o codigo, nao o que se explica).
  file(STRINGS "${file}" lines)
  set(code "")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "//.*$" "" line "${line}")
    string(REGEX REPLACE "^[ \t]*#[^i].*$" "" line "${line}")  # comentarios de CMake (# ...), nao #include
    string(APPEND code "${line}\n")
  endforeach()
  set(${out} "${code}" PARENT_SCOPE)
endfunction()

# 1. O Core.exe.
read_code(desktop_cmake "${SOURCE}/app/desktop/CMakeLists.txt")
if(desktop_cmake MATCHES "core_ai[ \t\n)]" OR desktop_cmake MATCHES "core_ai$")
  list(APPEND problems "app/desktop linka core_ai (a AI dentro do Core.exe)")
endif()
if(desktop_cmake MATCHES "core_sqlite|sqlite3")
  list(APPEND problems "app/desktop linka o SQLite (a memoria da AI e do processo da AI)")
endif()
file(GLOB desktop_sources "${SOURCE}/app/desktop/src/*.cpp" "${SOURCE}/app/desktop/src/*.h")
set(ai_headers ai_chat.hpp memory.hpp knowledge.hpp provider.hpp models.hpp stream.hpp kick_bass.hpp service.hpp sqlite3.h)
foreach(f IN LISTS desktop_sources)
  read_code(code "${f}")
  get_filename_component(name "${f}" NAME)
  foreach(h IN LISTS ai_headers)
    if(code MATCHES "#include[ \t]*[\"<]${h}[\">]")
      list(APPEND problems "${name} inclui ${h} (codigo da AI no Core.exe)")
    endif()
  endforeach()
  if(code MATCHES "ai::(Chat|Memory|Knowledge|Service|modelEntries|StreamAssembler)")
    list(APPEND problems "${name} usa ai::${CMAKE_MATCH_1} (codigo da AI no Core.exe)")
  endif()
endforeach()

# 2. O lado do Core da AI.
file(GLOB link_sources "${SOURCE}/app/ai-link/src/*")
foreach(f IN LISTS link_sources)
  read_code(code "${f}")
  get_filename_component(name "${f}" NAME)
  if(code MATCHES "#include[ \t]*[\"<](sys/socket\\.h|winsock2?\\.h|winhttp\\.h|wininet\\.h|curl/|sqlite3\\.h|ai_chat\\.hpp|memory\\.hpp|provider\\.hpp)")
    list(APPEND problems "app/ai-link/${name} inclui ${CMAKE_MATCH_1}")
  endif()
endforeach()
read_code(link_cmake "${SOURCE}/app/ai-link/CMakeLists.txt")
if(link_cmake MATCHES "core_ai[ \t\n)]|core_sqlite|juce::")
  list(APPEND problems "core_ai_link depende de ${CMAKE_MATCH_0}")
endif()

# 3. O motor.
file(GLOB_RECURSE engine_files "${SOURCE}/engine/src/*.cpp" "${SOURCE}/engine/src/*.hpp" "${SOURCE}/engine/src/*.h" "${SOURCE}/engine/CMakeLists.txt")
foreach(f IN LISTS engine_files)
  read_code(code "${f}")
  if(code MATCHES "core_ai|ai-link|ai_chat\\.hpp|ai_link\\.hpp|ai_protocol\\.hpp|ai_admit\\.hpp|ai_scope\\.hpp|catalog\\.hpp|ai::")
    file(RELATIVE_PATH rel "${SOURCE}" "${f}")
    list(APPEND problems "${rel} conhece a AI (${CMAKE_MATCH_0}): o caminho de audio tem de ficar sem ela")
  endif()
endforeach()

# 4. O painel: so pipes.
foreach(name AiPanel.cpp AiPanel.h)
  read_code(code "${SOURCE}/app/desktop/src/${name}")
  if(code MATCHES "createInputStream|WebInputStream|StreamingSocket|DatagramSocket|WinHttp|InternetOpen|withPOSTData")
    list(APPEND problems "${name} fala com a rede (${CMAKE_MATCH_0}): o Core so fala com a AI por pipes")
  endif()
endforeach()

# 5. O pacote do Core.
read_code(installer "${SOURCE}/app/installer/CMakeLists.txt")
if(installer MATCHES "CORE_AI_KNOWLEDGE_DB|core_ai_knowledge|core-ai|CoreAIGate|/ai[ /\"]|[ \t]ai[ \t\n]|app/ai/src")
  list(APPEND problems "o stage do instalador do Core leva a AI (${CMAKE_MATCH_0})")
endif()
read_code(setup "${SOURCE}/app/installer/Setup.cpp")
if(setup MATCHES "synth\\.hpp|core::ai::")
  list(APPEND problems "Setup.cpp do Core trata de coisas da AI (${CMAKE_MATCH_0})")
endif()
if(desktop_cmake MATCHES "CORE_AI_KNOWLEDGE_DB|core_ai_knowledge|/ai[ /\"]|core-ai")
  list(APPEND problems "a pasta da build do Core.exe ou o core-package levam a AI (${CMAKE_MATCH_0})")
endif()

# 6. A AI so com CORE_BUILD_AI: cada add_subdirectory dela dentro de um if(CORE_BUILD_AI).
file(STRINGS "${SOURCE}/CMakeLists.txt" root_lines)
set(guard_depth 0)
set(depth 0)
foreach(line IN LISTS root_lines)
  string(REGEX REPLACE "#.*$" "" line "${line}")
  if(line MATCHES "^[ 	]*if[(]")
    math(EXPR depth "${depth} + 1")
    if(line MATCHES "if[(]CORE_BUILD_AI[)]" AND guard_depth EQUAL 0)
      set(guard_depth ${depth})
    endif()
  elseif(line MATCHES "^[ 	]*endif[(]")
    if(depth EQUAL guard_depth)
      set(guard_depth 0)
    endif()
    math(EXPR depth "${depth} - 1")
  elseif(line MATCHES "add_subdirectory[(](app/ai|app/ai-process|third_party/sqlite|app/aifx)[)]" AND guard_depth EQUAL 0)
    list(APPEND problems "${CMAKE_MATCH_1} compila-se sem CORE_BUILD_AI (o Core tem de compilar sem a AI)")
  endif()
endforeach()

if(NOT problems STREQUAL "")
  foreach(p IN LISTS problems)
    message("  ${p}")
  endforeach()
  message(FATAL_ERROR "ai_boundary_static: a AI esta dentro do Core")
endif()
message(STATUS "ai_boundary_static: ok")
