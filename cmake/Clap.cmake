# CLAP headers (MIT), pinned to the 1.2.10 tag.
include(FetchContent)
FetchContent_Declare(clap
  GIT_REPOSITORY https://github.com/free-audio/clap.git
  GIT_TAG 195b42a004144fab0b3cf95e9c067187d15365b7 # 1.2.10
  SOURCE_SUBDIR helm-headers-only)
FetchContent_MakeAvailable(clap)
set(HELM_CLAP_INCLUDE "${clap_SOURCE_DIR}/include")
file(WRITE ${CMAKE_BINARY_DIR}/clap-source-path.txt "${clap_SOURCE_DIR}")
