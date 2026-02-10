cmake_minimum_required(VERSION 3.10)
project(earcut_hpp LANGUAGES CXX)

add_library(earcut_hpp INTERFACE)
add_library(earcut_hpp::earcut_hpp ALIAS earcut_hpp)
target_include_directories(earcut_hpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
