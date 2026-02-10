add_library(fsst-lib STATIC
    "${PROJECT_SOURCE_DIR}/vendor/fsst/libfsst.cpp"
    "${PROJECT_SOURCE_DIR}/vendor/fsst/fsst_avx512.cpp"
)
target_include_directories(fsst-lib PUBLIC "${PROJECT_SOURCE_DIR}/vendor/fsst")
set_target_properties(fsst-lib PROPERTIES CXX_STANDARD 17)
target_compile_options(fsst-lib PRIVATE -w)

target_link_libraries(mlt-cpp-encoder fsst-lib)
target_include_directories(mlt-cpp-encoder PRIVATE "${PROJECT_SOURCE_DIR}/vendor/fsst")

list(APPEND MLT_EXPORT_TARGETS fsst-lib)
