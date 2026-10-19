include(${CMAKE_SOURCE_DIR}/cmake/Sources.cmake)
set(wam_sources ${HELM_COMMON_SOURCES})
list(FILTER wam_sources INCLUDE REGEX "^(mopo/src/|src/synthesis/|src/common/helm_common.cpp)")
list(TRANSFORM wam_sources PREPEND "${CMAKE_SOURCE_DIR}/")
add_executable(HelmWamDsp ${wam_sources}
  ${CMAKE_SOURCE_DIR}/src/browser/WamDsp.cpp
  ${CMAKE_SOURCE_DIR}/src/browser/BrowserAllocationAudit.c)
target_include_directories(HelmWamDsp PRIVATE ${CMAKE_SOURCE_DIR}/mopo/src
  ${CMAKE_SOURCE_DIR}/src/synthesis ${CMAKE_SOURCE_DIR}/src/common)
target_compile_features(HelmWamDsp PRIVATE cxx_std_17)
target_link_options(HelmWamDsp PRIVATE --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1
  -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=134217728
  -sSTACK_SIZE=2097152 "-sEXPORTED_RUNTIME_METHODS=['ccall','UTF8ToString','HEAPF32']")
foreach(function malloc calloc realloc free posix_memalign aligned_alloc memalign)
  target_link_options(HelmWamDsp PRIVATE -Wl,--wrap=${function})
endforeach()
set_target_properties(HelmWamDsp PROPERTIES OUTPUT_NAME dsp SUFFIX .mjs
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web/wam)

find_program(HELM_NODE_EXECUTABLE node REQUIRED)
add_custom_target(HelmWamAssets ALL
  COMMAND ${HELM_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/scripts/build-wam.mjs ${CMAKE_BINARY_DIR}/web/wam
  DEPENDS HelmWamDsp
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_dependencies(HelmBrowser HelmWamAssets)
