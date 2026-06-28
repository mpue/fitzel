cmake_minimum_required(VERSION 3.21)

project(Fitzel
    VERSION 0.1.0
    DESCRIPTION "Portable OpenGL base for a graphics engine"
    LANGUAGES C CXX)

# --- Global settings -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all runtime artifacts in one place so assets/shaders resolve predictably.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Default to a sensible build type for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Generate compile_commands.json for tooling (clangd, etc.).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FITZEL_BUILD_SANDBOX "Build the sandbox example application" ON)

# --- Third-party dependencies ----------------------------------------------
include(cmake/Dependencies.cmake)

# --- Project targets -------------------------------------------------------
add_subdirectory(engine)

if(FITZEL_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()
