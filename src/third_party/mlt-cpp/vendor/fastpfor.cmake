if(NOT MLT_WITH_FASTPFOR)
  message(STATUS "[MLT] No FastPFOR support")
  return()
endif(MLT_WITH_FASTPFOR)


message(STATUS "[MLT] Including FastPFOR support")

# FastPFor
set(FASTPFOR_WITH_TEST OFF CACHE BOOL "Disable tests in FastPFor" FORCE) # The fastpfor gtest targets conflict with ours

# SUPPORT_NEON results in trying to link simde (`-lsimde`) but it's a header-only library
set(SUPPORT_NEON OFF CACHE BOOL "" FORCE)

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
    target_compile_definitions(${_target} PUBLIC MLT_ENABLE_FASTPFOR=1)
endforeach()
list(APPEND MLT_EXPORT_TARGETS FastPFOR)
