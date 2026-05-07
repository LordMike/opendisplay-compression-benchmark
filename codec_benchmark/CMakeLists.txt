cmake_minimum_required(VERSION 3.16)
project(compressor_benchmark C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)

set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps")
set(HEATSHRINK_DIR "${DEPS_DIR}/heatshrink")
set(G5_DIR "${DEPS_DIR}/g5_imageconvert")

if(NOT EXISTS "${HEATSHRINK_DIR}/heatshrink_encoder.c" OR NOT EXISTS "${G5_DIR}/Group5.h")
    message(FATAL_ERROR "Missing dependencies. Run `make deps` in experiments/compressor_benchmark first.")
endif()

add_library(heatshrink STATIC
    "${HEATSHRINK_DIR}/heatshrink_encoder.c"
    "${HEATSHRINK_DIR}/heatshrink_decoder.c"
)
target_include_directories(heatshrink PUBLIC "${HEATSHRINK_DIR}")
target_compile_definitions(heatshrink PUBLIC HEATSHRINK_DYNAMIC_ALLOC=1 HEATSHRINK_USE_INDEX=1)

add_executable(compressor_benchmark
    src/main.cpp
    src/g5_f64.cpp
    src/g5_f128.cpp
    src/g5_f256.cpp
)
target_include_directories(compressor_benchmark PRIVATE "${G5_DIR}" "${HEATSHRINK_DIR}")
target_link_libraries(compressor_benchmark PRIVATE heatshrink ZLIB::ZLIB)

add_executable(fixture_generator src/fixture_generator.cpp)

