set(EARCUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_VIZ OFF CACHE BOOL "" FORCE)

add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/earcut" "${CMAKE_CURRENT_BINARY_DIR}/earcut" EXCLUDE_FROM_ALL SYSTEM)

target_link_libraries(mlt-cpp-encoder earcut_hpp)
