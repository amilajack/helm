if(NOT DEFINED BUILD_DIR OR NOT DEFINED CONFIG)
  message(FATAL_ERROR "Pass -DBUILD_DIR=<build directory> -DCONFIG=Debug|Release")
endif()
foreach(relative
    "HelmPlugin_artefacts/${CONFIG}/VST3/Helm.vst3"
    "HelmPlugin_artefacts/${CONFIG}/AU/Helm.component"
    "HelmStandalone_artefacts/${CONFIG}/Helm.app")
  set(bundle "${BUILD_DIR}/${relative}")
  set(binary "${bundle}/Contents/MacOS/Helm")
  execute_process(COMMAND /usr/bin/lipo -archs "${binary}"
    OUTPUT_VARIABLE architectures OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  if(NOT architectures STREQUAL "arm64")
    message(FATAL_ERROR "Unexpected architectures in ${binary}: ${architectures}")
  endif()
  execute_process(COMMAND /usr/bin/codesign --verify --strict "${bundle}"
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND /usr/bin/xcrun vtool -show-build "${binary}"
    OUTPUT_VARIABLE build_version COMMAND_ERROR_IS_FATAL ANY)
  if(NOT build_version MATCHES "minos 11[.]0")
    message(FATAL_ERROR "Unexpected deployment target in ${binary}: ${build_version}")
  endif()
  if(NOT IS_DIRECTORY "${bundle}/Contents/Resources/patches/Factory Presets")
    message(FATAL_ERROR "Missing bundled presets in ${bundle}")
  endif()
  message(STATUS "Verified arm64, macOS 11, signature and presets: ${relative}")
endforeach()

# JUCE 5's getUUIDForVST2ID uses "VST"/"VSE", the Helm code, and the lower-case name.
file(READ "${BUILD_DIR}/HelmPlugin_artefacts/${CONFIG}/VST3/Helm.vst3/Contents/Resources/moduleinfo.json" info)
foreach(cid "56535448656C6D68656C6D0000000000" "56534548656C6D68656C6D0000000000")
  if(NOT info MATCHES "${cid}")
    message(FATAL_ERROR "Legacy VST3 class ID missing: ${cid}")
  endif()
endforeach()
