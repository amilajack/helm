cmake_minimum_required(VERSION 3.22)
if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Helm supports native Apple Silicon")
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum macOS version")
endif()
project(Helm VERSION 0.9.0 LANGUAGES C CXX)
option(HELM_DSP_PARITY_ONLY "Build the DSP parity renderer without JUCE" OFF)
option(HELM_CLAP_ONLY "Build only the native CLAP plugin, without JUCE" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "WASI" OR HELM_CLAP_ONLY)
  enable_testing()
  add_subdirectory(clap)
  return()
endif()
if(HELM_DSP_PARITY_ONLY)
  include(cmake/DspParity.cmake)
  return()
endif()
if(EMSCRIPTEN)
  add_subdirectory(web)
  return()
endif()
enable_language(OBJC OBJCXX)
if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  message(FATAL_ERROR "This CMake build targets arm64 macOS. Use the legacy projects for other platforms.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/JUCE9/CMakeLists.txt")
  set(FETCHCONTENT_SOURCE_DIR_JUCE "${CMAKE_SOURCE_DIR}/third_party/JUCE9")
endif()
include(FetchContent)
FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 3fb5cbc0340758596dac39f7c8de47d562b4896e)
FetchContent_MakeAvailable(juce)
include(cmake/Sources.cmake)
juce_add_binary_data(HelmResources SOURCES ${HELM_RESOURCES})
juce_add_plugin(HelmPlugin
  COMPANY_NAME "Matt Tytel" BUNDLE_ID org.tytel.helmplugin
  PLUGIN_MANUFACTURER_CODE Tyte PLUGIN_CODE Helm
  FORMATS VST3 AU PRODUCT_NAME Helm
  IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE AU_EXPORT_PREFIX helm
  VST3_CATEGORIES Instrument Synth COPY_PLUGIN_AFTER_BUILD FALSE)
juce_add_gui_app(HelmStandalone PRODUCT_NAME Helm
  COMPANY_NAME "Matt Tytel" BUNDLE_ID org.tytel.helm
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "Helm uses audio devices for standalone playback.")
foreach(target HelmPlugin HelmStandalone)
  juce_generate_juce_header(${target})
  target_sources(${target} PRIVATE ${HELM_COMMON_SOURCES})
  target_compile_options(${target} PRIVATE -Wno-deprecated-declarations)
  target_include_directories(${target} PRIVATE
    concurrentqueue mopo/src src src/common src/editor_components
    src/editor_sections src/look_and_feel src/synthesis src/standalone)
  target_compile_definitions(${target} PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_VST3_CAN_REPLACE_VST2=1 JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
    JUCE_IGNORE_VST3_MISMATCHED_PARAMETER_ID_WARNING=1)
  target_link_libraries(${target} PRIVATE HelmResources
    juce::juce_audio_utils juce::juce_gui_extra juce::juce_opengl)
endforeach()
target_sources(HelmPlugin PRIVATE ${HELM_PLUGIN_SOURCES})
target_sources(HelmStandalone PRIVATE ${HELM_STANDALONE_SOURCES})

foreach(target HelmPlugin_VST3 HelmPlugin_AU HelmStandalone)
  juce_add_bundle_resources_directory(${target} patches)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:${target}>"
    VERBATIM)
endforeach()

include(CTest)
add_subdirectory(clap)
if(BUILD_TESTING)
  juce_add_console_app(HelmPluginTests PRODUCT_NAME HelmPluginTests)
  target_sources(HelmPluginTests PRIVATE tests/plugin_smoke.cpp)
  target_compile_definitions(HelmPluginTests PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(HelmPluginTests PRIVATE juce::juce_audio_utils)
  add_dependencies(HelmPluginTests HelmPlugin_VST3 HelmPlugin_AU HelmStandalone)
  add_test(NAME vst3-smoke COMMAND HelmPluginTests
    "$<TARGET_BUNDLE_DIR:HelmPlugin_VST3>"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-parameters.txt")
  add_test(NAME au-smoke COMMAND HelmPluginTests
    "$<TARGET_BUNDLE_DIR:HelmPlugin_AU>"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-parameters.txt")
  add_test(NAME standalone-version COMMAND "$<TARGET_FILE:HelmStandalone>" --version)
  set_tests_properties(standalone-version PROPERTIES PASS_REGULAR_EXPRESSION "Helm 0.9.0")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-home")
  set_tests_properties(vst3-smoke au-smoke standalone-version PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
    ENVIRONMENT "HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home;CFFIXED_USER_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home")
endif()
