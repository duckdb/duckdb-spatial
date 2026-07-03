if(NOT MLT_WITH_FASTPFOR)
  message(STATUS "[MLT] No FastPFOR support")
  return()
endif(NOT MLT_WITH_FASTPFOR)

message(STATUS "[MLT] Including FastPFOR support")

# The fastpfor gtest targets conflict with ours
set(FASTPFOR_WITH_TEST OFF CACHE BOOL "Disable tests in FastPFor" FORCE)

add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/fastpfor" "${CMAKE_CURRENT_BINARY_DIR}/fastpfor" EXCLUDE_FROM_ALL SYSTEM)

# Disable all warnings for FastPFOR
if(MSVC)
    target_compile_options(FastPFOR PRIVATE /w)
else()
    target_compile_options(FastPFOR PRIVATE -w)
endif()

foreach(_target mlt-cpp mlt-cpp-encoder)
    target_link_libraries(${_target} FastPFOR)
    target_include_directories(${_target} PRIVATE SYSTEM "${PROJECT_SOURCE_DIR}/vendor/fastpfor/headers")
    target_compile_definitions(${_target} PUBLIC MLT_WITH_FASTPFOR=1)
    if(MLT_WITH_FASTPFOR_SIMD)
        target_compile_definitions(${_target} PUBLIC MLT_WITH_FASTPFOR_SIMD=1)
    endif(MLT_WITH_FASTPFOR_SIMD)
endforeach()

list(APPEND MLT_EXPORT_TARGETS FastPFOR)
