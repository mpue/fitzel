cmake_minimum_required(VERSION 3.21)

project(Fitzel
    VERSION 0.4.0
    DESCRIPTION "Game engine and editor for things that move through a landscape"
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

# --- Version header ---------------------------------------------------------
# x.y.z come from project() above; the fourth number is the commit count, filled
# in by a build-time step so it tracks new commits without re-running CMake.
# Everything that wants it includes <fitzel/Version.hpp>.
set(FITZEL_VERSION_DIR ${CMAKE_BINARY_DIR}/generated/fitzel)
add_custom_target(fitzel_version ALL
    COMMAND ${CMAKE_COMMAND}
            -DFITZEL_VERSION=${PROJECT_VERSION}
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -DIN_FILE=${CMAKE_SOURCE_DIR}/cmake/Version.hpp.in
            -DOUT_FILE=${FITZEL_VERSION_DIR}/Version.hpp
            -P ${CMAKE_SOURCE_DIR}/cmake/VersionGen.cmake
    BYPRODUCTS ${FITZEL_VERSION_DIR}/Version.hpp
    COMMENT "Stamping version")
# Generate it once at configure time too, so the header exists for tooling
# (compile_commands.json consumers) before the first build.
execute_process(COMMAND ${CMAKE_COMMAND}
    -DFITZEL_VERSION=${PROJECT_VERSION}
    -DSRC_DIR=${CMAKE_SOURCE_DIR}
    -DIN_FILE=${CMAKE_SOURCE_DIR}/cmake/Version.hpp.in
    -DOUT_FILE=${FITZEL_VERSION_DIR}/Version.hpp
    -P ${CMAKE_SOURCE_DIR}/cmake/VersionGen.cmake)

# --- Third-party dependencies ----------------------------------------------
include(cmake/Dependencies.cmake)

# --- Project targets -------------------------------------------------------
add_subdirectory(engine)

if(FITZEL_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()
