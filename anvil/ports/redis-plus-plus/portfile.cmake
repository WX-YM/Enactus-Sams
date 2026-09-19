vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sewenew/redis-plus-plus
    REF "${VERSION}"
    SHA512 3de216fc32894eb2d9f61a559bf69c8e154122209f2ba95aac202d769688d27cd1059424ad8a1173c7073ee34bfbd5ad981bb313d9298cd39ebe245e88d9e9fe
    HEAD_REF master
    PATCHES
        fix-conversion.patch
        fix-dependency-libuv.patch
        fix-absolute-path.patch
)

# redis-plus-plus compiles its own sources with a hardcoded -Werror. GCC 15 and
# 16 emit a false-positive -Wmaybe-uninitialized when a constructor calls a
# member function from its member-initialiser list, which several of this
# library's exception types do, so the build fails outright.
#
# Injecting -Wno-error via VCPKG_CXX_FLAGS does not work: those flags land
# BEFORE the target's own "-Wall -Wextra -Werror", which then re-enables the
# warning and re-promotes it. The flag has to come out of the CMakeLists.
#
# This relaxes nothing in anvil's own code — our flags come from
# cmake/AnvilHardening.cmake and are unaffected. Drop this overlay once the
# upstream port builds cleanly on a current GCC.
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
    [["-Wall" "-Wextra" "-Werror"]]
    [["-Wall" "-Wextra"]])

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        "tls"   REDIS_PLUS_PLUS_USE_TLS
)

if("cxx17" IN_LIST FEATURES)
    set(REDIS_PLUS_PLUS_CXX_STANDARD 17)
else()
    set(REDIS_PLUS_PLUS_CXX_STANDARD 11)
endif()

set(EXTRA_OPT "")
if ("async" IN_LIST FEATURES)
    list(APPEND EXTRA_OPT "-DREDIS_PLUS_PLUS_BUILD_ASYNC=libuv")
endif()
if ("async-std" IN_LIST FEATURES)
    list(APPEND EXTRA_OPT "-DREDIS_PLUS_PLUS_ASYNC_FUTURE=std")
endif()

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" REDIS_PLUS_PLUS_BUILD_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" REDIS_PLUS_PLUS_BUILD_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_PARALLEL_CONFIGURE
    OPTIONS
        ${FEATURE_OPTIONS}
        -DREDIS_PLUS_PLUS_BUILD_STATIC=${REDIS_PLUS_PLUS_BUILD_STATIC}
        -DREDIS_PLUS_PLUS_BUILD_SHARED=${REDIS_PLUS_PLUS_BUILD_SHARED}
        -DREDIS_PLUS_PLUS_BUILD_TEST=OFF
        -DREDIS_PLUS_PLUS_CXX_STANDARD=${REDIS_PLUS_PLUS_CXX_STANDARD}
        ${EXTRA_OPT}
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(PACKAGE_NAME redis++ CONFIG_PATH share/cmake/redis++)

if("async" IN_LIST FEATURES)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/redis++/redis++-config.cmake"
"include(CMakeFindDependencyMacro)"
[[include(CMakeFindDependencyMacro)
find_dependency(libuv CONFIG)]])
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Handle copyright
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

vcpkg_fixup_pkgconfig()
