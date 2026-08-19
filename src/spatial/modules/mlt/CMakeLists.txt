add_library(mlt_module OBJECT mlt_module.cpp)
set_target_properties(mlt_module PROPERTIES CXX_STANDARD 20)

target_include_directories(mlt_module PRIVATE
    ${SPATIAL_ROOT_DIR}/src
    ${SPATIAL_ROOT_DIR}/src/third_party/mlt-cpp/include
)

set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
    $<TARGET_OBJECTS:mlt_module>
    PARENT_SCOPE)
