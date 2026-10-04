include("${VCPKG_ROOT_DIR}/ports/${PORT}/portfile.cmake")

vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg_install_meson.cmake"
    "COMMAND \"\${NINJA}\" install -v"
    "COMMAND \"\${NINJA}\" -v install"
)
