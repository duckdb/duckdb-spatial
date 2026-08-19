cmake_minimum_required(VERSION 3.10)
project(FastPFOR LANGUAGES C CXX)

find_path(SIMDE_INCLUDE_DIR simde/x86/sse4.1.h REQUIRED)

add_library(FastPFOR STATIC
    src/bitpacking.cpp
    src/bitpackingaligned.cpp
    src/bitpackingunaligned.cpp
    src/horizontalbitpacking.cpp
    src/simdunalignedbitpacking.cpp
    src/codecfactory.cpp
    src/simdbitpacking.cpp
    src/varintdecode.c
    src/streamvbyte.c
)

target_include_directories(FastPFOR
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/headers>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${SIMDE_INCLUDE_DIR}
)
target_compile_features(FastPFOR PUBLIC cxx_std_11)
target_compile_definitions(FastPFOR PUBLIC SIMDE_ENABLE_NATIVE_ALIASES)
set_target_properties(FastPFOR PROPERTIES POSITION_INDEPENDENT_CODE ON)

if((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86") AND NOT MSVC)
    target_compile_options(FastPFOR PRIVATE -msse4.2)
endif()

install(TARGETS FastPFOR
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY headers/ DESTINATION include)
