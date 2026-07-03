vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO fast-pack/FastPFOR
    REF "2be1f976935b8ff9296b029f574d7f964be9d35d"
    SHA512 "fb966b46327573c092e184d1e2cc4ad6b09485086b4ac64c6cc716ad475eaeb205dd39cd8c4a9a799992f70cb9b7ed220335c671f41edc699100d262fda70ad4"
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)
vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
