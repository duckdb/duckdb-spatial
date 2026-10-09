vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OSGeo/gdal
    REF "v${VERSION}"
    SHA512 3b915c38cc7c9eb139df9335a90a2f6fd123c54e19ecb1f22670400eac76e317c5334f95dff5875c9c3fde8a0ef0f7aea86fa58ad42afaee823a46c6616e9c17
    HEAD_REF master
    PATCHES
        find-link-libraries.patch
        iconv.diff
        libarchive.diff
        libkml.patch
        sqlite3.diff
        target-is-valid.patch
        duckdb_gdal_json.patch
)
file(REMOVE "${SOURCE_PATH}/cmake/modules/packages/FindIconv.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/modules/packages/FindZSTD.cmake")
# `vcpkg clean` stumbles over one subdir
file(REMOVE_RECURSE "${SOURCE_PATH}/autotest")

# Avoid abseil, no matter if vcpkg or system
vcpkg_replace_string("${SOURCE_PATH}/ogr/ogrsf_frmts/flatgeobuf/flatbuffers/base.h" [[__has_include("absl/strings/string_view.h")]] "(0)")

# Cf. cmake/helpers/CheckDependentLibraries.cmake
# The default for all `GDAL_USE_<PKG>` dependencies is `OFF`.
# Here, we explicitly control dependencies provided via vpcpkg.
vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        network          SPATIAL_USE_NETWORK
        geos             SPATIAL_USE_GEOS
)

# Compatibility with older Android versions https://github.com/OSGeo/gdal/pull/5941
if(VCPKG_TARGET_IS_ANDROID AND ANDROID_PLATFORM VERSION_LESS 24 AND (VCPKG_TARGET_ARCHITECTURE STREQUAL "x86" OR VCPKG_TARGET_ARCHITECTURE STREQUAL "arm"))
    list(APPEND FEATURE_OPTIONS -DBUILD_WITHOUT_64BIT_OFFSET=ON)
endif()

# Doesnt work on wasm32
if(NOT VCPKG_TARGET_ARCHITECTURE STREQUAL "wasm32")
    set(OGR_ENABLE_DRIVER_OPENFILEGDB ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DVCPKG_HOST_TRIPLET=${HOST_TRIPLET} # for host pkgconf in PATH
        -DBUILD_DOCS=OFF
        # GDAL Options
        -DGDAL_OBJECT_LIBRARIES_POSITION_INDEPENDENT_CODE=ON # this is needed for GDAL to build with -fPIC
        -DBUILD_TESTING=OFF
        -DBUILD_APPS=OFF

        # Arrow
        -DGDAL_USE_ARROW=OFF
        -DARROW_USE_STATIC_LIBRARIES=OFF

        # Disable all external drivers unless explicitly enabled
        -DGDAL_USE_EXTERNAL_LIBS=OFF
        -DGDAL_USE_INTERNAL_LIBS=ON

        # These are required
        -DGDAL_USE_ZLIB=ON
        -DGDAL_USE_ZLIB_INTERNAL=OFF # Dont use zlib bundled

        # Disable these
        -DGDAL_USE_PNG=OFF
        -DGDAL_USE_JPEG=OFF
        -DGDAL_USE_PNG_INTERNAL=OFF
        -DGDAL_USE_JPEG_INTERNAL=OFF
        -DGDAL_USE_GIF=OFF
        -DGDAL_USE_GIF_INTERNAL=OFF
        -DGDAL_USE_QHULL=OFF
        -DGDAL_USE_QHULL_INTERNAL=OFF
        -DGDAL_USE_LERC=OFF
        -DGDAL_USE_LERC_INTERNAL=OFF

        # Supported drivers
        -DGDAL_USE_GEOS=${SPATIAL_USE_GEOS}
        -DGDAL_USE_SQLITE3=ON
        -DGDAL_USE_EXPAT=ON
        -DGDAL_USE_CURL=${SPATIAL_USE_NETWORK}
        -DGDAL_USE_OPENSSL=${SPATIAL_USE_NETWORK}
        -DOPENSSL_USE_STATIC_LIBS=ON # Propagate to FindOpenSSL.cmake

        # This is not true, but a bug in gdal's cmake files
        -DACCEPT_MISSING_SQLITE3_RTREE:BOOL=ON
        -DACCEPT_MISSING_SQLITE3_MUTEX_ALLOC:BOOL=ON

        # Remove optional gdal drivers
        -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF
        -DOGR_BUILD_OPTIONAL_DRIVERS=OFF

        # Special issues
        -DCMAKE_DISABLE_FIND_PACKAGE_Arrow=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_HDF5=ON
        -DGDAL_USE_HDF5=OFF
        -DGDAL_USE_LIBKML=OFF

        # Build these explicitly
        # (Compared to gdal 3.8.5, the MEM driver is now always built-in, and
        # the NTF, TIGER, GEOCONCEPT and SVG drivers have been removed upstream)
        -DOGR_ENABLE_DRIVER_GEOJSON=ON
        -DOGR_ENABLE_DRIVER_GML=ON
        -DOGR_ENABLE_DRIVER_TAB=ON
        -DOGR_ENABLE_DRIVER_SHAPE=ON
        -DOGR_ENABLE_DRIVER_KML=ON
        -DOGR_ENABLE_DRIVER_VRT=ON
        -DOGR_ENABLE_DRIVER_AVC=ON
        -DOGR_ENABLE_DRIVER_LVBAG=ON
        -DOGR_ENABLE_DRIVER_S57=ON
        -DOGR_ENABLE_DRIVER_CSV=ON
        -DOGR_ENABLE_DRIVER_DGN=ON
        -DOGR_ENABLE_DRIVER_GMT=ON
        -DOGR_ENABLE_DRIVER_GEORSS=ON
        -DOGR_ENABLE_DRIVER_DXF=ON
        -DOGR_ENABLE_DRIVER_PGDUMP=ON
        -DOGR_ENABLE_DRIVER_GPSBABEL=ON
        -DOGR_ENABLE_DRIVER_EDIGEO=ON
        -DOGR_ENABLE_DRIVER_SXF=ON
        -DOGR_ENABLE_DRIVER_OPENFILEGDB=${OGR_ENABLE_DRIVER_OPENFILEGDB}
        -DOGR_ENABLE_DRIVER_WASP=ON
        -DOGR_ENABLE_DRIVER_SELAFIN=ON
        -DOGR_ENABLE_DRIVER_JML=ON
        -DOGR_ENABLE_DRIVER_VDV=ON
        -DOGR_ENABLE_DRIVER_FLATGEOBUF=ON
        -DOGR_ENABLE_DRIVER_MAPML=ON
        -DOGR_ENABLE_DRIVER_GPX=ON
        -DOGR_ENABLE_DRIVER_SQLITE=ON
        -DOGR_ENABLE_DRIVER_GPKG=ON
        -DOGR_ENABLE_DRIVER_OSM=ON
        -DOGR_ENABLE_DRIVER_XLSX=ON
        -DOGR_ENABLE_DRIVER_CAD=ON
        -DOGR_ENABLE_DRIVER_ODS=ON
        -DOGR_ENABLE_DRIVER_VFK=ON
        -DOGR_ENABLE_DRIVER_MVT=ON
        -DOGR_ENABLE_DRIVER_PMTILES=ON
        -DOGR_ENABLE_DRIVER_JSONFG=ON
        -DOGR_ENABLE_DRIVER_GTFS=ON

        # Drivers requiring network/curl
        -DOGR_ENABLE_DRIVER_AMIGOCLOUD=${SPATIAL_USE_NETWORK}
        -DOGR_ENABLE_DRIVER_CARTO=${SPATIAL_USE_NETWORK}
        -DOGR_ENABLE_DRIVER_WFS=${SPATIAL_USE_NETWORK}
        -DOGR_ENABLE_DRIVER_NGW=${SPATIAL_USE_NETWORK}
        -DOGR_ENABLE_DRIVER_ELASTIC=${SPATIAL_USE_NETWORK}
        -DOGR_ENABLE_DRIVER_CSW=${SPATIAL_USE_NETWORK}
        -DOGR_ENABLE_DRIVER_PLSCENES=${SPATIAL_USE_NETWORK}

        # Remove bindings
        -DBUILD_PYTHON_BINDINGS=OFF
    OPTIONS_DEBUG
        -DBUILD_APPS=OFF
    MAYBE_UNUSED_VARIABLES
        ACCEPT_MISSING_SQLITE3_MUTEX_ALLOC
        ACCEPT_MISSING_SQLITE3_RTREE
        ARROW_USE_STATIC_LIBRARIES
        OPENSSL_USE_STATIC_LIBS
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/gdal)
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/gdal/GDALConfig.cmake"
    "include(CMakeFindDependencyMacro)"
    "include(CMakeFindDependencyMacro)
# gdal needs a pkg-config tool. A host dependency provides pkgconf.
get_filename_component(vcpkg_host_prefix \"\${CMAKE_CURRENT_LIST_DIR}/../../../${HOST_TRIPLET}\" ABSOLUTE)
list(APPEND CMAKE_PROGRAM_PATH \"\${vcpkg_host_prefix}/tools/pkgconf\")"
)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(REMOVE "${CURRENT_PACKAGES_DIR}/bin/gdal-config" "${CURRENT_PACKAGES_DIR}/debug/bin/gdal-config")

file(GLOB bin_files "${CURRENT_PACKAGES_DIR}/bin/*")
if(NOT bin_files)
    file(REMOVE_RECURSE
        "${CURRENT_PACKAGES_DIR}/bin"
        "${CURRENT_PACKAGES_DIR}/debug/bin"
    )
endif()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/cpl_config.h" "#define GDAL_PREFIX \"${CURRENT_PACKAGES_DIR}\"" "")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.TXT")
