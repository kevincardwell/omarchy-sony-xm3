cmake_minimum_required(VERSION 3.20)

project(omarchy-sony-xm3
    VERSION 0.1.0
    DESCRIPTION "Sony WH-1000XM3 Headless Bluetooth Daemon, CLI, and Omarchy Widget"
    LANGUAGES CXX C
)

# Standard C++20 enforcement
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd and language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default installation directory to ~/.local
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default install prefix to ~/.local" FORCE)
endif()

# Compiler options: Strict warnings as required
add_compile_options(
    -Wall
    -Wextra
    -pedantic
    -Wno-unused-parameter
)

# Position independent code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Testing configuration
include(CTest)
enable_testing()

# Daemon & Protocol Library Subdirectory
add_subdirectory(daemon)

# CLI Subdirectory
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt")
    add_subdirectory(cli)
endif()

# Stress / boundary harnesses
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
