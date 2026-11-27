# PresentationEngine
# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.25)
project(PresentationEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "")
endif()

option(ENGINE_BUILD_METAL "Build the Metal backend, text/media seams and engined" ${APPLE})
option(ENGINE_BUILD_TESTS "Build engine_tests" ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()

# ---- Protocol: generate C++ from proto/ at build time ----------------------
file(GLOB ENGINE_PROTOS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/proto/engine/v1/*.proto
  ${CMAKE_SOURCE_DIR}/proto/library/v1/*.proto
  ${CMAKE_SOURCE_DIR}/proto/importer/v1/*.proto
  ${CMAKE_SOURCE_DIR}/tests/scenes/reference_scene.proto)
add_library(engine_proto STATIC)
target_link_libraries(engine_proto PUBLIC protobuf::libprotobuf)
target_include_directories(engine_proto PUBLIC ${CMAKE_BINARY_DIR}/proto_gen)
protobuf_generate(TARGET engine_proto LANGUAGE cpp PROTOS ${ENGINE_PROTOS}
  IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto ${CMAKE_SOURCE_DIR}/tests/scenes
  PROTOC_OUT_DIR ${CMAKE_BINARY_DIR}/proto_gen)
target_compile_options(engine_proto PRIVATE -w)

# ---- engine_core: portable C++, no Apple headers ---------------------------
add_library(engine_core STATIC
  src/core/clock.cpp
  src/core/projection.cpp
  src/core/document_index.cpp
  src/core/live_state.cpp
  src/core/resolve.cpp
  src/core/frames_in_flight.cpp
  src/core/reference_scene.cpp
  src/text/rtf.cpp)
target_include_directories(engine_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(engine_core PUBLIC engine_proto)

# Allocation and lock-wait counters. Linked into executables (it replaces
# global operator new), never into libraries other programs might load.
add_library(engine_alloc_tracking OBJECT src/core/alloc_tracking.cpp)
target_include_directories(engine_alloc_tracking PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---- engine_rhi: the GPU seam (interface only) ------------------------------
add_library(engine_rhi INTERFACE)
target_include_directories(engine_rhi INTERFACE ${CMAKE_SOURCE_DIR}/src)

# ---- engine_render: render list -> RHI calls (portable) ---------------------
add_library(engine_render STATIC src/render/compositor.cpp src/render/resources.cpp)
target_link_libraries(engine_render PUBLIC engine_core engine_rhi)

if(ENGINE_BUILD_METAL)
  include(cmake/MetalCpp.cmake)
  include(cmake/SlangShaders.cmake)

  engine_slang_to_msl(ENGINE_CORE_SHADERS
    shaders/core/composite.slang vs_quad
    shaders/core/composite.slang fs_composite
    shaders/core/convert.slang cs_to_bgra8
    shaders/core/convert.slang cs_to_uyvy
    shaders/core/convert.slang cs_to_v210)

  # Platform seams for text layout (Core Text) and still-image decode (ImageIO).
  add_library(engine_text_apple STATIC src/text/coretext_layout.cpp src/media/image_decode_apple.cpp)
  target_link_libraries(engine_text_apple PUBLIC engine_core
    "-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation" "-framework ImageIO")

  add_library(rhi_metal STATIC src/rhi_metal/metal_device.cpp src/rhi_metal/metal_cpp_impl.cpp ${ENGINE_CORE_SHADERS})
  target_link_libraries(rhi_metal PUBLIC engine_rhi engine_core metal_cpp
    "-framework Metal" "-framework QuartzCore" "-framework Foundation")

  add_executable(engined src/engined/main.cpp src/engined/scene_runner.cpp src/engined/png_writer.cpp
    src/engined/preview_window.mm $<TARGET_OBJECTS:engine_alloc_tracking>)
  target_link_libraries(engined PRIVATE engine_render rhi_metal engine_text_apple "-framework AppKit")
  set_source_files_properties(src/engined/preview_window.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

if(ENGINE_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
  add_executable(engine_tests
    tests/engine/clock_test.cpp
    tests/engine/projection_test.cpp
    tests/engine/command_queue_test.cpp
    tests/engine/resolve_test.cpp
    tests/engine/frames_in_flight_test.cpp
    tests/engine/rtf_test.cpp
    tests/engine/convert_reference_test.cpp
    $<TARGET_OBJECTS:engine_alloc_tracking>)
  target_link_libraries(engine_tests PRIVATE engine_core GTest::gtest_main)
  target_compile_definitions(engine_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(ENGINE_BUILD_METAL)
    target_sources(engine_tests PRIVATE tests/engine/gpu_test.cpp tests/engine/text_layout_test.cpp src/engined/scene_runner.cpp src/engined/png_writer.cpp)
    target_link_libraries(engine_tests PRIVATE engine_render rhi_metal engine_text_apple)
  endif()
  include(GoogleTest)
  gtest_discover_tests(engine_tests DISCOVERY_TIMEOUT 60)
endif()
