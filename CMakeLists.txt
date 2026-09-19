cmake_minimum_required(VERSION 3.24)
project(EnactusSAMSBackend CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Anvil configuration
set(ANVIL_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/config")
add_subdirectory(anvil)

# Add our backend executable
add_executable(enactus_backend
    src/main.cc
    src/handlers/api.cc
)
target_link_libraries(enactus_backend PRIVATE anvil::app)

add_executable(emit_descriptor src/emit_descriptor.cc)
target_link_libraries(emit_descriptor PRIVATE anvil::foundation)
