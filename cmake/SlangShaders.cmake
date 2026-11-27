# Offline shader compilation: Slang -> MSL with tools/slang_msl, then the MSL
# text is embedded in a generated .cpp. The Metal backend builds a library from
# it at startup (no Xcode Metal toolchain needed at build time).
#
# The Slang compiler comes from the slangpy wheel the contract checks already
# install (`pip install slangpy`); it ships slang.h and libslang-compiler.
# Override with -DSLANG_DIR=<dir containing include/slang.h and the dylib>.

if(NOT SLANG_DIR)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import slangpy, os; print(os.path.dirname(slangpy.__file__))"
    OUTPUT_VARIABLE SLANG_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _slang_rc)
  if(NOT _slang_rc EQUAL 0)
    message(FATAL_ERROR "Slang not found: pip install slangpy, or pass -DSLANG_DIR")
  endif()
endif()
file(GLOB SLANG_LIB "${SLANG_DIR}/libslang-compiler*.dylib" "${SLANG_DIR}/libslang-compiler*.so*")
list(GET SLANG_LIB 0 SLANG_LIB)
message(STATUS "Slang: ${SLANG_LIB}")

add_executable(slang_msl tools/slang_msl/main.cpp)
target_include_directories(slang_msl PRIVATE ${SLANG_DIR}/include)
target_link_libraries(slang_msl PRIVATE ${SLANG_LIB})
set_target_properties(slang_msl PROPERTIES BUILD_RPATH ${SLANG_DIR})

# engine_slang_to_msl(<out_var> <file> <entry> [<file> <entry>]...)
# Produces one .metal per entry point and a shaders_msl.cpp exposing
#   engine::shaders::find_msl("<entry>") -> const char*
function(engine_slang_to_msl out_var)
  set(args ${ARGN})
  set(gen_dir ${CMAKE_BINARY_DIR}/shaders)
  set(metal_files "")
  set(entries "")
  list(LENGTH args n)
  math(EXPR last "${n} - 1")
  foreach(i RANGE 0 ${last} 2)
    math(EXPR j "${i} + 1")
    list(GET args ${i} src)
    list(GET args ${j} entry)
    set(out ${gen_dir}/${entry}.metal)
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
      COMMAND slang_msl ${CMAKE_SOURCE_DIR}/${src} ${entry} ${out} -I ${CMAKE_SOURCE_DIR}/shaders
      DEPENDS slang_msl ${CMAKE_SOURCE_DIR}/${src}
      COMMENT "Slang -> MSL ${entry}")
    list(APPEND metal_files ${out})
    list(APPEND entries ${entry})
  endforeach()
  set(cpp ${gen_dir}/shaders_msl.cpp)
  string(REPLACE ";" "," entry_csv "${entries}")
  add_custom_command(OUTPUT ${cpp}
    COMMAND ${CMAKE_COMMAND} -DOUT=${cpp} -DDIR=${gen_dir} -DENTRIES=${entry_csv}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedMsl.cmake
    DEPENDS ${metal_files} ${CMAKE_SOURCE_DIR}/cmake/EmbedMsl.cmake
    COMMENT "Embedding MSL")
  set(${out_var} ${cpp} PARENT_SCOPE)
endfunction()
