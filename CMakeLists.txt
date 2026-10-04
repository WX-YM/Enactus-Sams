cmake_minimum_required(VERSION 3.24)
project(EnactusSAMSBackend CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# anvil is a source dependency (anvil docs/02-getting-started.md §1): it is
# compiled against this application's tables in src/config.
set(ANVIL_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/config")
set(ANVIL_BUILD_TESTS OFF CACHE BOOL "anvil's own suite is built from anvil/ standalone" FORCE)
set(ANVIL_WITH_EGY ON CACHE BOOL "Egyptian mobile numbers on the application form" FORCE)
add_subdirectory(anvil)

option(ENACTUS_BUILD_TESTS "Build the backend's test suite" ON)

# Everything the backend is made of, shared by the server, the migration tool
# and the tests. An OBJECT library rather than a static one: anvil's job
# registry references the handlers in src/app/jobs.cc, and a static archive
# linked before anvil would leave them unresolved.
file(GLOB ENACTUS_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cc")
add_library(enactus_core OBJECT ${ENACTUS_CORE_SOURCES})
target_include_directories(enactus_core PUBLIC src src/config)
target_link_libraries(enactus_core PUBLIC anvil::app)
anvil_target(enactus_core)

add_executable(enactus_backend src/main.cc)
target_link_libraries(enactus_backend PRIVATE enactus_core)
anvil_target(enactus_backend)

# Indexes, collection options and the one-shot legacy import. Run from one place
# per deploy, never at boot (anvil docs/00-architecture.md §7.7).
add_executable(enactus_migrate src/migrate.cc)
target_link_libraries(enactus_migrate PRIVATE enactus_core)
anvil_target(enactus_migrate)

# Writes the descriptor hammer generates the admin panel's client from.
add_executable(emit_descriptor src/emit_descriptor.cc)
target_include_directories(emit_descriptor PRIVATE src src/config)
target_link_libraries(emit_descriptor PRIVATE anvil::app)
anvil_target(emit_descriptor)

if(ENACTUS_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(enactus_tests
        tests/tables_test.cc
        tests/legacy_test.cc
    )
    target_link_libraries(enactus_tests PRIVATE enactus_core GTest::gtest_main)
    anvil_target(enactus_tests)
    include(GoogleTest)
    gtest_discover_tests(enactus_tests DISCOVERY_TIMEOUT 60)
endif()
