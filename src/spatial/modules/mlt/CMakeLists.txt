add_library(mlt_module OBJECT mlt_module.cpp)

set_target_properties(mlt_module PROPERTIES CXX_STANDARD 20)

target_include_directories(mlt_module PRIVATE
    ${SPATIAL_ROOT_DIR}/src
    ${SPATIAL_ROOT_DIR}/src/third_party/mlt-cpp/include
    ${SPATIAL_ROOT_DIR}/src/third_party/yyjson/include
    ${SPATIAL_ROOT_DIR}/src/third_party/protozero/include
    ${SPATIAL_ROOT_DIR}/src/third_party/shapelib
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/third_party/fsst
    ${CMAKE_SOURCE_DIR}/third_party/fmt/include
    ${CMAKE_SOURCE_DIR}/third_party/hyperloglog
    ${CMAKE_SOURCE_DIR}/third_party/fastpforlib
    ${CMAKE_SOURCE_DIR}/third_party/skiplist
    ${CMAKE_SOURCE_DIR}/third_party/ska_sort
    ${CMAKE_SOURCE_DIR}/third_party/fast_float
    ${CMAKE_SOURCE_DIR}/third_party/re2
    ${CMAKE_SOURCE_DIR}/third_party/miniz
    ${CMAKE_SOURCE_DIR}/third_party/utf8proc/include
    ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue
    ${CMAKE_SOURCE_DIR}/third_party/pcg
    ${CMAKE_SOURCE_DIR}/third_party/pdqsort
    ${CMAKE_SOURCE_DIR}/third_party/tdigest
    ${CMAKE_SOURCE_DIR}/third_party/mbedtls/include
    ${CMAKE_SOURCE_DIR}/third_party/jaro_winkler
    ${CMAKE_SOURCE_DIR}/third_party/vergesort
    ${CMAKE_SOURCE_DIR}/third_party/yyjson/include
    ${CMAKE_SOURCE_DIR}/third_party/zstd/include
)

target_compile_definitions(mlt_module PRIVATE
    DUCKDB_BUILD_LIBRARY
    DUCKDB_MAJOR_VERSION=${DUCKDB_MAJOR_VERSION}
    DUCKDB_MINOR_VERSION=${DUCKDB_MINOR_VERSION}
    DUCKDB_PATCH_VERSION=${DUCKDB_PATCH_VERSION}
    DUCKDB_SPATIAL_EXTENSION=1
)

if(MSVC)
    target_compile_options(mlt_module PRIVATE /w)
else()
    target_compile_options(mlt_module PRIVATE -w)
endif()

set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
    $<TARGET_OBJECTS:mlt_module>
    PARENT_SCOPE)
