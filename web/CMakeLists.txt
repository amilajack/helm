# The browser uses the same source manifest and pinned JUCE as the native build.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/JUCE9/CMakeLists.txt")
  set(FETCHCONTENT_SOURCE_DIR_JUCE_SOURCE "${CMAKE_SOURCE_DIR}/third_party/JUCE9")
endif()
include(FetchContent)
FetchContent_Declare(juce_source
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 3fb5cbc0340758596dac39f7c8de47d562b4896e
  SOURCE_SUBDIR helm-browser-no-native-tools)
FetchContent_MakeAvailable(juce_source)
file(WRITE ${CMAKE_BINARY_DIR}/juce-source-path.txt "${juce_source_SOURCE_DIR}")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(overlay ${CMAKE_BINARY_DIR}/juce-browser)
set(browser ${CMAKE_SOURCE_DIR}/src/browser)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare-juce.py
  ${juce_source_SOURCE_DIR} ${overlay} COMMAND_ERROR_IS_FATAL ANY)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-pthread -Wno-deprecated-declarations -Wno-inconsistent-missing-override)
add_link_options(-pthread)
add_library(HelmBrowserJuce STATIC)
foreach(module juce_core juce_events juce_data_structures juce_graphics juce_gui_basics
  juce_audio_basics juce_audio_formats juce_audio_devices juce_audio_processors_headless
  juce_audio_processors juce_audio_utils juce_gui_extra)
  # JUCE 9 splits large modules and bundled codecs into additional translation units.
  file(GLOB module_sources CONFIGURE_DEPENDS
    ${overlay}/modules/${module}/${module}*.cpp ${overlay}/modules/${module}/${module}*.c)
  target_sources(HelmBrowserJuce PRIVATE ${module_sources})
endforeach()
target_include_directories(HelmBrowserJuce PUBLIC ${overlay}/modules ${browser})
target_compile_definitions(HelmBrowserJuce PUBLIC
  SUPPORT_AFFINITIES=0 JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
  JUCE_LITTLE_ENDIAN=1 JUCE_32BIT=1
  JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_FREETYPE=1 JUCE_USE_FONTCONFIG=0
  JUCE_MODAL_LOOPS_PERMITTED=0 JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0
  JUCE_USE_XSHM=0 JUCE_USE_XCURSOR=0 JUCE_ALSA=0 JUCE_JACK=0
  JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0
  JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_LV2=0 PAY_NAG=0)
target_compile_options(HelmBrowserJuce PRIVATE -sUSE_FREETYPE=1)
target_link_options(HelmBrowserJuce INTERFACE -sUSE_FREETYPE=1)
include(${CMAKE_SOURCE_DIR}/cmake/Sources.cmake)
list(REMOVE_ITEM HELM_COMMON_SOURCES src/look_and_feel/shaders.cpp)
list(TRANSFORM HELM_COMMON_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/")
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/binary-data.py
  ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/binary COMMAND_ERROR_IS_FATAL ANY)
add_executable(HelmBrowser ${HELM_COMMON_SOURCES}
  ${CMAKE_CURRENT_BINARY_DIR}/binary/BinaryData.cpp
  ${browser}/BrowserFiles.cpp ${browser}/BrowserMain.cpp ${browser}/BrowserAudio.cpp ${browser}/BrowserControl.cpp
  ${browser}/BrowserAudioThread.c ${browser}/BrowserAllocationAudit.c
  ${CMAKE_SOURCE_DIR}/src/standalone/helm_computer_keyboard.cpp)
target_include_directories(HelmBrowser PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/binary
  ${CMAKE_SOURCE_DIR}/concurrentqueue ${CMAKE_SOURCE_DIR}/mopo/src ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/common ${CMAKE_SOURCE_DIR}/src/editor_components
  ${CMAKE_SOURCE_DIR}/src/editor_sections ${CMAKE_SOURCE_DIR}/src/look_and_feel
  ${CMAKE_SOURCE_DIR}/src/synthesis ${CMAKE_SOURCE_DIR}/src/standalone)
target_link_libraries(HelmBrowser PRIVATE HelmBrowserJuce)
set_source_files_properties(${browser}/BrowserAudioThread.c PROPERTIES INCLUDE_DIRECTORIES
  "${EMSCRIPTEN_ROOT_PATH}/system/lib/libc/musl/src/include;${EMSCRIPTEN_ROOT_PATH}/system/lib/libc/musl/src/internal;${EMSCRIPTEN_ROOT_PATH}/system/lib/libc/musl/arch/emscripten;${EMSCRIPTEN_ROOT_PATH}/system/lib/pthread")
target_link_options(HelmBrowser PRIVATE --profiling-funcs -sAUDIO_WORKLET=1 -sWASM_WORKERS=1
  -sINITIAL_MEMORY=536870912 -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=2097152
  -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM=1
  --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html
  "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/browser/platform.js"
  "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/browser/factory-startup.js" -lidbfs.js
  "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','stringToNewUTF8','UTF8ToString']")
foreach(function malloc calloc realloc free posix_memalign aligned_alloc memalign)
  target_link_options(HelmBrowser PRIVATE -Wl,--wrap=${function})
endforeach()
set_target_properties(HelmBrowser PROPERTIES OUTPUT_NAME helm SUFFIX .html
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web)
set_property(TARGET HelmBrowser APPEND PROPERTY LINK_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/shell.html ${CMAKE_CURRENT_SOURCE_DIR}/browser/platform.js)
foreach(script clipboard text-input accessibility audio-lifecycle midi transport user-files app)
  configure_file(browser/${script}.js ${CMAKE_BINARY_DIR}/web/${script}.js COPYONLY)
endforeach()

add_custom_target(HelmFactoryLibrary
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/factory-library.py
    ${CMAKE_SOURCE_DIR}/patches ${CMAKE_BINARY_DIR}/web/library)
add_dependencies(HelmBrowser HelmFactoryLibrary)
set_property(TARGET HelmBrowser APPEND PROPERTY LINK_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/browser/factory-startup.js)

add_custom_target(HelmBrowserFonts
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fallback-fonts.py
    ${CMAKE_BINARY_DIR}/web/fonts-fallback)
add_dependencies(HelmBrowser HelmBrowserFonts)
