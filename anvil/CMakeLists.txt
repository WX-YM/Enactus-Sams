cmake_minimum_required(VERSION 3.24)

# The vcpkg toolchain must be set before project(). CMakePresets.json does this;
# a manual configure needs -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
project(anvil
    VERSION 0.1.0
    DESCRIPTION "A hardened C++20 web-application library for Drogon + MongoDB + Redis"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Consumable via add_subdirectory: a parent project's PROJECT_SOURCE_DIR is its
# own, so every path inside anvil is anchored to this variable instead.
set(ANVIL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "anvil root")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AnvilHardening)

option(ANVIL_BUILD_TESTS "Build unit tests and benchmarks" ON)
option(ANVIL_WITH_VIPS   "Build the image subsystem (requires system libvips)" ON)
option(ANVIL_WITH_EGY    "Build the Egyptian identity/phone validators" OFF)
option(ANVIL_WITH_RE2    "Link RE2. std::regex stays banned regardless" OFF)

# ---------------------------------------------------------------------------
# The application configuration header
#
# anvil is a SOURCE dependency, not a prebuilt archive. The locale table
# dimensions std::array members inside anvil's own translation units — the
# tier-1 section cache is array<array<atomic<shared_ptr<...>>, N>, K> — so N has
# to be a literal at the point anvil is compiled, not a runtime span
# (docs/01-seams.md §2).
#
# A FATAL_ERROR here rather than a missing-header error two hundred translation
# units into the build: the failure is a configuration mistake and it should
# read as one.
# ---------------------------------------------------------------------------
if(NOT DEFINED ANVIL_CONFIG_INCLUDE_DIR)
    if(ANVIL_BUILD_TESTS)
        # Standalone build: the test app is the reference consumer and supplies
        # the same header any real application would.
        set(ANVIL_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/testapp")
        message(STATUS "anvil: ANVIL_CONFIG_INCLUDE_DIR unset, using tests/testapp")
    else()
        message(FATAL_ERROR
            "ANVIL_CONFIG_INCLUDE_DIR is not set.\n"
            "Point it at the directory holding your anvil_app_config.h:\n"
            "  set(ANVIL_CONFIG_INCLUDE_DIR \${CMAKE_CURRENT_SOURCE_DIR}/src/config)\n"
            "See docs/01-seams.md §2.")
    endif()
endif()
set(ANVIL_CONFIG_INCLUDE_DIR "${ANVIL_CONFIG_INCLUDE_DIR}" CACHE INTERNAL "app config dir")

# ---------------------------------------------------------------------------
# Dependencies
#
# Everything below comes from vcpkg except libvips, which has no vcpkg port and
# is taken from the system via pkg-config. See docs/08-images.md.
# ---------------------------------------------------------------------------
find_package(Drogon         CONFIG REQUIRED)
find_package(mongocxx       CONFIG REQUIRED)
find_package(bsoncxx        CONFIG REQUIRED)
find_package(redis++        CONFIG REQUIRED)
find_package(OpenSSL               REQUIRED COMPONENTS SSL Crypto)
find_package(ICU                   REQUIRED COMPONENTS uc i18n data)
find_package(simdutf        CONFIG REQUIRED)
find_package(xxHash         CONFIG REQUIRED)

# libargon2 ships only a pkg-config file.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)

if(ANVIL_WITH_VIPS)
    pkg_check_modules(VIPS IMPORTED_TARGET vips-cpp)
    if(NOT VIPS_FOUND)
        message(FATAL_ERROR
            "libvips not found and it has no vcpkg port.\n"
            "  Arch/CachyOS : sudo pacman -S libvips\n"
            "  Debian/Ubuntu: sudo apt install libvips-dev\n"
            "  Fedora       : sudo dnf install vips-devel\n"
            "Or configure with -DANVIL_WITH_VIPS=OFF to skip the image subsystem.")
    endif()
endif()

if(ANVIL_WITH_RE2)
    find_package(re2 CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# anvil's own recorder: the bytes of every failure this library can produce.
#
# It links anvil::foundation and nothing else, which is the same property that
# makes generating a client need no driver, no event loop and no database — and
# it is built whether or not the tests are, because its output is a RELEASE
# artefact (docs/15-tasks.md phase 12). The reference DESCRIPTOR beside it in a
# release comes from testapp_emit_descriptor, which needs the reference
# application and therefore needs ANVIL_BUILD_TESTS; this one does not, and the
# asymmetry is the library/application line showing through.
add_executable(anvil_emit_envelopes tools/emit_envelopes.cc)
anvil_target(anvil_emit_envelopes)
target_link_libraries(anvil_emit_envelopes PRIVATE anvil::foundation)

if(ANVIL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "anvil ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  sanitizers   : ${ANVIL_SANITIZERS}")
message(STATUS "  app config   : ${ANVIL_CONFIG_INCLUDE_DIR}")
message(STATUS "  image subsys : ${ANVIL_WITH_VIPS}")
message(STATUS "  egypt locale : ${ANVIL_WITH_EGY}")
message(STATUS "  tests        : ${ANVIL_BUILD_TESTS}")
message(STATUS "")
