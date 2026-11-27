# metal-cpp: Apple's header-only C++ wrapper for Metal (Apache-2.0).
# Fetched from Apple at configure time and pinned by hash; not vendored.
include(FetchContent)
FetchContent_Declare(metal_cpp_src
  URL https://developer.apple.com/metal/cpp/files/metal-cpp_26.zip
  URL_HASH SHA256=4df3c078b9aadcb516212e9cb03004cbc5ce9a3e9c068fa3144d021db585a3a4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(metal_cpp_src)

add_library(metal_cpp INTERFACE)
target_include_directories(metal_cpp SYSTEM INTERFACE ${metal_cpp_src_SOURCE_DIR})
