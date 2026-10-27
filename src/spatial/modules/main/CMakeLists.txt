set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_functions_cast.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_functions_scalar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_functions_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_functions_aggregate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spatial_functions_window.cpp
    PARENT_SCOPE)