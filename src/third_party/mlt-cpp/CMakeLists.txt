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
# Use DuckDB's existing FSST target.
target_link_libraries(mlt-cpp-encoder duckdb_fsst)
target_include_directories(mlt-cpp-encoder BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/third_party/fsst")
target_compile_definitions(mlt-cpp-encoder PRIVATE
    fsst_encoder_t=duckdb_fsst_encoder_t
    fsst_decoder_t=duckdb_fsst_decoder_t
    fsst_create=duckdb_fsst_create
    fsst_destroy=duckdb_fsst_destroy
    fsst_compress=duckdb_fsst_compress
    fsst_export=duckdb_fsst_export
    fsst_decoder=duckdb_fsst_decoder
)

# --- Earcut (encoder dependency) ---
add_library(earcut_hpp INTERFACE)
target_include_directories(earcut_hpp INTERFACE "${MLT_SOURCE_DIR}/vendor/earcut/include")
target_link_libraries(mlt-cpp-encoder earcut_hpp)

# --- FastPFOR (decoder + encoder dependency) ---
find_path(FASTPFOR_INCLUDE_DIR fastpfor.h REQUIRED)
find_path(SIMDE_INCLUDE_DIR simde/x86/sse4.1.h REQUIRED)
find_library(FASTPFOR_LIBRARY NAMES FastPFOR REQUIRED)
foreach(_target mlt-cpp mlt-cpp-encoder)
    target_link_libraries(${_target} ${FASTPFOR_LIBRARY})
    target_include_directories(${_target} PRIVATE ${FASTPFOR_INCLUDE_DIR} ${SIMDE_INCLUDE_DIR})
    target_compile_definitions(${_target} PUBLIC MLT_WITH_FASTPFOR=1 MLT_WITH_JSON=0 SIMDE_ENABLE_NATIVE_ALIASES)
endforeach()
