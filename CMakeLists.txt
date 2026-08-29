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

# --- Symbols in the Release build ------------------------------------------
# The editor ships /SUBSYSTEM:WINDOWS with no console, so what a crash leaves
# behind is a minidump in %LOCALAPPDATA%\CrashDumps and nothing else. Without a
# PDB that dump names an address and stops there: reading it means finding the
# module base by hand and working backwards from the offset to guess what was
# being dereferenced. With one, tools/crashdump.py prints function and line.
#
# /Zi does not change the code the optimiser produces -- it only writes down
# what it did. The one thing that WOULD change the binary is the linker's habit
# of switching /OPT:REF and /OPT:ICF off as soon as /DEBUG appears, so both are
# named again here: same executable as before, plus a file beside it that can
# explain it.
if(MSVC)
    string(APPEND CMAKE_C_FLAGS_RELEASE   " /Zi")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /Zi")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE    " /DEBUG /OPT:REF /OPT:ICF")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
endif()

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

# --- AddressSanitizer (the `asan` preset, a tree of its own) ----------------
# What this is for: a use-after-free does not crash where the mistake is. It
# crashes later, wherever the allocator happens to hand the block out again --
# which is how a brush stroke that read a freed component came down as a crash
# on the NEXT stroke, at an address that had to be reverse-engineered before it
# named anything. ASan turns that into a report at the read itself, with the
# stacks for the allocation and for the free beside it.
#
# Deliberately NOT applied to the dependencies. ASan replaces the allocator for
# the whole process, so heap errors are caught in uninstrumented code too --
# what compiling a translation unit adds on top is stack and global checking.
# Buying that for Jolt, assimp and imgui would cost most of the build time and
# hand back other people's findings, so this starts where our own code starts.
# It sits after the dependencies for exactly that reason: add_compile_options
# only reaches targets defined below it.
#
# Release, not Debug: /RTC1 and ASan are incompatible, and /O2 keeps it fast
# enough to leave on. The runtime (clang_rt.asan_dynamic-x86_64.dll) ships with
# MSVC and vcvars puts it on PATH; cl writes the link directive for it into the
# objects, so nothing has to be said here about linking.
option(FITZEL_ASAN "Instrument our own code with AddressSanitizer" OFF)
if(FITZEL_ASAN)
    if(MSVC)
        # /bigobj because instrumentation adds sections per function, and
        # main.cpp is fourteen thousand lines: without it the object file
        # overflows its own format (C1128). Worth reading as a measurement
        # of that file rather than as a compiler flag.
        add_compile_options(/fsanitize=address /bigobj)
        add_link_options(/INCREMENTAL:NO)
        # The price of not instrumenting the dependencies. Under ASan the
        # MSVC STL turns on container annotations -- red zones between a
        # vector's size and its capacity -- and stamps a /FAILIFMISMATCH
        # into every object file, so a translation unit that has them
        # cannot be linked against one that does not. assimp and Jolt do
        # not, and the link comes apart in several hundred LNK2038s.
        #
        # Switching them off costs one narrow check: a read between size()
        # and capacity(), inside memory the allocator really did hand out.
        # Everything the sanitizer is here for is untouched, because ASan
        # replaces the allocator itself -- use-after-free, double free,
        # reads past the end of an allocation, stack overruns. To buy the
        # narrow check back, move this block above the dependencies so they
        # are instrumented too, and drop this definition.
        add_compile_definitions(_DISABLE_STL_ANNOTATION)
        # The runtime beside the executables, not merely somewhere on a
        # developer prompt's PATH. Without it they do not start at all --
        # and a process that fails to load exits 0xC0000135, which is
        # NEGATIVE, so a runner testing `if errorlevel 1` calls that a pass.
        # Eight harnesses once reported green that way without executing a
        # single instruction.
        get_filename_component(_fitzel_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_file(FITZEL_ASAN_RUNTIME clang_rt.asan_dynamic-x86_64.dll
                  HINTS "${_fitzel_msvc_bin}" NO_DEFAULT_PATH)
        if(FITZEL_ASAN_RUNTIME)
            file(COPY "${FITZEL_ASAN_RUNTIME}"
                 DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        else()
            message(WARNING "ASan runtime not found next to the compiler; "
                            "the instrumented binaries will not start.")
        endif()
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "Fitzel: AddressSanitizer on (engine + sandbox)")
endif()

# --- Project targets -------------------------------------------------------
add_subdirectory(engine)

if(FITZEL_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()
