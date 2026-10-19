# Helm as a CLAP plugin: WCLAP (wasm32-wasip1 via wasi-sdk) or a native .clap.
include(${CMAKE_SOURCE_DIR}/cmake/Clap.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Sources.cmake)
set(dsp_sources ${HELM_COMMON_SOURCES})
list(FILTER dsp_sources INCLUDE REGEX "^(mopo/src/|src/synthesis/|src/common/helm_common.cpp)")
list(TRANSFORM dsp_sources PREPEND "${CMAKE_SOURCE_DIR}/")

find_program(HELM_NODE_EXECUTABLE node REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(gui_cpp ${CMAKE_CURRENT_BINARY_DIR}/WebviewResources.cpp)
file(GLOB gui_inputs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/web/wclap/* ${CMAKE_SOURCE_DIR}/web/wam/*.ts)
add_custom_command(OUTPUT ${gui_cpp}
  COMMAND ${HELM_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/scripts/build-wclap-gui.mjs ${gui_cpp}
  DEPENDS ${gui_inputs} ${CMAKE_SOURCE_DIR}/web/scripts/build-wclap-gui.mjs
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

set(clap_sources ${dsp_sources}
  ${CMAKE_SOURCE_DIR}/src/headless/HelmInstrument.cpp
  ${CMAKE_SOURCE_DIR}/src/clap/HelmState.cpp
  ${CMAKE_SOURCE_DIR}/src/clap/HelmClap.cpp
  ${gui_cpp})
set(clap_includes ${HELM_CLAP_INCLUDE} ${CMAKE_SOURCE_DIR}/mopo/src ${CMAKE_SOURCE_DIR}/src/synthesis
  ${CMAKE_SOURCE_DIR}/src/common ${CMAKE_SOURCE_DIR}/src/headless ${CMAKE_SOURCE_DIR}/src/clap)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
  set(bundle ${CMAKE_BINARY_DIR}/Helm.wclap)
  add_executable(HelmWclap ${clap_sources} ${CMAKE_SOURCE_DIR}/src/browser/BrowserAllocationAudit.c)
  target_include_directories(HelmWclap PRIVATE ${clap_includes})
  target_compile_features(HelmWclap PRIVATE cxx_std_17)
  target_compile_options(HelmWclap PRIVATE -msimd128 -fno-exceptions -fvisibility=hidden)
  # Reactor module: no main; host calls _initialize, then clap_entry.
  target_link_options(HelmWclap PRIVATE -mexec-model=reactor
    -Wl,--export=clap_entry
    -Wl,--export-table -Wl,--growable-table
    -Wl,--initial-memory=67108864 -Wl,--max-memory=268435456 -Wl,-z,stack-size=2097152)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(HelmWclap PRIVATE -Wl,--strip-debug)
  endif()
  foreach(function malloc calloc realloc free posix_memalign aligned_alloc)
    target_link_options(HelmWclap PRIVATE -Wl,--wrap=${function})
  endforeach()
  set_target_properties(HelmWclap PROPERTIES OUTPUT_NAME module SUFFIX .wasm
    RUNTIME_OUTPUT_DIRECTORY ${bundle})
  add_custom_target(HelmWclapBundle ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/scripts/factory-library.py
      ${CMAKE_SOURCE_DIR}/patches ${bundle}/library
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/scripts/package-wclap.py
      ${bundle} ${CMAKE_BINARY_DIR}/Helm.wclap.tar.gz ${HELM_CLAP_INCLUDE}/../LICENSE
    DEPENDS HelmWclap
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  return()
endif()

add_library(HelmClap MODULE ${clap_sources} ${CMAKE_SOURCE_DIR}/src/headless/AllocationStub.cpp)
target_include_directories(HelmClap PRIVATE ${clap_includes})
target_compile_features(HelmClap PRIVATE cxx_std_17)
set_target_properties(HelmClap PROPERTIES OUTPUT_NAME Helm PREFIX "" C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden)
if(APPLE)
  target_link_libraries(HelmClap PRIVATE "-framework Accelerate")
  set_target_properties(HelmClap PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION clap
    MACOSX_BUNDLE_GUI_IDENTIFIER org.tytel.helm.clap MACOSX_BUNDLE_BUNDLE_NAME Helm
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)
  add_custom_command(TARGET HelmClap POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/scripts/factory-library.py
      ${CMAKE_SOURCE_DIR}/patches "$<TARGET_BUNDLE_DIR:HelmClap>/Contents/Resources/library"
    COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:HelmClap>"
    VERBATIM)
  set(helm_clap_path "$<TARGET_BUNDLE_DIR:HelmClap>")
else()
  set_target_properties(HelmClap PROPERTIES SUFFIX .clap)
  set(helm_clap_path "$<TARGET_FILE:HelmClap>")
endif()

enable_testing()
add_executable(HelmClapTests ${CMAKE_SOURCE_DIR}/tests/clap_host_test.cpp)
target_include_directories(HelmClapTests PRIVATE ${HELM_CLAP_INCLUDE})
target_compile_features(HelmClapTests PRIVATE cxx_std_17)
target_link_libraries(HelmClapTests PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(HelmClapTests HelmClap)
add_test(NAME clap-host COMMAND HelmClapTests "$<TARGET_FILE:HelmClap>" "${helm_clap_path}"
  "${CMAKE_SOURCE_DIR}/tests/legacy-parameters.txt")
