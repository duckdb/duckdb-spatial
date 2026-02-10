cmake_minimum_required(VERSION 3.25)
project(mlt-cpp LANGUAGES CXX)

set(MLT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# --- Decoder ---
add_library(mlt-cpp STATIC
    ${MLT_SOURCE_DIR}/src/mlt/decoder.cpp
    ${MLT_SOURCE_DIR}/src/mlt/decode/int.cpp
    ${MLT_SOURCE_DIR}/src/mlt/feature.cpp
    ${MLT_SOURCE_DIR}/src/mlt/geometry_vector.cpp
    ${MLT_SOURCE_DIR}/src/mlt/layer.cpp
    ${MLT_SOURCE_DIR}/src/mlt/metadata/stream.cpp
    ${MLT_SOURCE_DIR}/src/mlt/metadata/tileset.cpp
    ${MLT_SOURCE_DIR}/src/mlt/properties.cpp
    ${MLT_SOURCE_DIR}/src/mlt/util/rle.cpp
)

set_target_properties(mlt-cpp PROPERTIES CXX_STANDARD 20)
target_include_directories(mlt-cpp
    PUBLIC ${MLT_SOURCE_DIR}/include
    PRIVATE ${MLT_SOURCE_DIR}/src
)
target_include_directories(mlt-cpp PRIVATE "${MLT_SOURCE_DIR}/vendor/simde")

if(MSVC)
    target_compile_options(mlt-cpp PRIVATE /w)
else()
    target_compile_options(mlt-cpp PRIVATE -w)
endif()

# --- Encoder ---
add_library(mlt-cpp-encoder STATIC
    ${MLT_SOURCE_DIR}/src/mlt/encoder.cpp
    ${MLT_SOURCE_DIR}/src/mlt/encode/int.cpp
    ${MLT_SOURCE_DIR}/src/mlt/metadata/stream.cpp
    ${MLT_SOURCE_DIR}/src/mlt/metadata/tileset.cpp
)

set_target_properties(mlt-cpp-encoder PROPERTIES CXX_STANDARD 20)
target_include_directories(mlt-cpp-encoder
    PUBLIC ${MLT_SOURCE_DIR}/include
    PRIVATE ${MLT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(mlt-cpp-encoder PRIVATE /w)
else()
    target_compile_options(mlt-cpp-encoder PRIVATE -w)
endif()

# --- FSST (encoder dependency) ---
add_library(fsst-lib STATIC
    "${MLT_SOURCE_DIR}/vendor/fsst/libfsst.cpp"
    "${MLT_SOURCE_DIR}/vendor/fsst/fsst_avx512.cpp"
)
target_include_directories(fsst-lib PUBLIC "${MLT_SOURCE_DIR}/vendor/fsst")
set_target_properties(fsst-lib PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_options(fsst-lib PRIVATE /w)
else()
    target_compile_options(fsst-lib PRIVATE -w)
endif()
target_link_libraries(mlt-cpp-encoder fsst-lib)
target_include_directories(mlt-cpp-encoder PRIVATE "${MLT_SOURCE_DIR}/vendor/fsst")

# --- Earcut (encoder dependency) ---
set(EARCUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_VIZ OFF CACHE BOOL "" FORCE)
add_subdirectory("${MLT_SOURCE_DIR}/vendor/earcut" "${CMAKE_CURRENT_BINARY_DIR}/earcut" EXCLUDE_FROM_ALL)
target_link_libraries(mlt-cpp-encoder earcut_hpp)

# --- FastPFOR (decoder + encoder dependency) ---
set(FASTPFOR_WITH_TEST OFF CACHE BOOL "Disable tests in FastPFor" FORCE)
set(SUPPORT_NEON OFF CACHE BOOL "" FORCE)
add_subdirectory("${MLT_SOURCE_DIR}/vendor/fastpfor" "${CMAKE_CURRENT_BINARY_DIR}/fastpfor" EXCLUDE_FROM_ALL)
if(MSVC)
    target_compile_options(FastPFOR PRIVATE /w)
else()
    target_compile_options(FastPFOR PRIVATE -w)
endif()

foreach(_target mlt-cpp mlt-cpp-encoder)
    target_link_libraries(${_target} FastPFOR)
    target_include_directories(${_target} PRIVATE SYSTEM "${MLT_SOURCE_DIR}/vendor/fastpfor/headers")
    target_compile_definitions(${_target} PUBLIC MLT_ENABLE_FASTPFOR=1)
endforeach()
