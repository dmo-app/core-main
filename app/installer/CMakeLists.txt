# Core Setup.exe (M6): one file that installs Core for the current user (no administrator rights).
# The package (Core.exe, core-engine.exe, the demo, the templates, the languages, the patterns, PCore's styles, the AI's
# knowledge base in ai/) is built here and carried inside it.
#   cmake --build build --target CoreSetup   ->  build/app/installer/CoreSetup_artefacts/Release/Core Setup.exe
set(CORE_STAGE ${CMAKE_CURRENT_BINARY_DIR}/stage)
set(CORE_PACKAGE_ZIP ${CMAKE_CURRENT_BINARY_DIR}/CorePackage.zip)
add_custom_command(OUTPUT ${CORE_PACKAGE_ZIP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CORE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CORE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CoreApp> $<TARGET_FILE:core-engine> $<TARGET_FILE_DIR:CoreApp>/demo.core-document.json ${CORE_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/templates ${CORE_STAGE}/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lang ${CORE_STAGE}/lang
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/patterns ${CORE_STAGE}/patterns
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/pcore ${CORE_STAGE}/pcore
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CORE_STAGE}/ai
  COMMAND ${CMAKE_COMMAND} -E copy ${CORE_AI_KNOWLEDGE_DB} ${CORE_STAGE}/ai/core-ai-knowledge.sqlite
  COMMAND ${CMAKE_COMMAND} -E chdir ${CORE_STAGE} ${CMAKE_COMMAND} -E tar cf ${CORE_PACKAGE_ZIP} --format=zip
          Core.exe core-engine.exe demo.core-document.json templates lang patterns pcore ai
  DEPENDS CoreApp core-engine core_ai_knowledge ${CORE_AI_KNOWLEDGE_DB} ${CMAKE_SOURCE_DIR}/app/desktop/demo.core-document.json
  COMMENT "Packing the Core package for the installer")
juce_add_binary_data(CoreSetupData HEADER_NAME CoreSetupData.h NAMESPACE CoreSetupData SOURCES ${CORE_PACKAGE_ZIP})

juce_add_gui_app(CoreSetup
  PRODUCT_NAME "Core Setup"
  COMPANY_NAME "Core"
  VERSION ${CORE_VERSION}
  NEEDS_WEB_BROWSER FALSE
  NEEDS_CURL FALSE)
target_sources(CoreSetup PRIVATE Setup.cpp)
target_compile_definitions(CoreSetup PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:CoreApp,JUCE_VERSION>")  # the version of Core it installs
target_link_libraries(CoreSetup PRIVATE CoreSetupData juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags)

