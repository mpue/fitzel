# Generates Version.hpp from the project version + the current git state.
#
# Run in script mode (cmake -P) from a build-time custom target rather than at
# configure time, so the build number follows new commits without anyone having
# to re-run CMake. configure_file only rewrites the header when its contents
# actually change, so this does not force a rebuild on every build.
#
# Expects: FITZEL_VERSION (x.y.z), SRC_DIR (repo root), IN_FILE, OUT_FILE.

string(REPLACE "." ";" _parts "${FITZEL_VERSION}")
list(GET _parts 0 FITZEL_VER_MAJOR)
list(GET _parts 1 FITZEL_VER_MINOR)
list(GET _parts 2 FITZEL_VER_PATCH)

# Defaults for a build without git (source archive, or git not on PATH): a build
# number of 0 marks "not from a known commit" rather than faking one.
set(FITZEL_VER_BUILD 0)
set(FITZEL_GIT_HASH "")
set(FITZEL_GIT_DIRTY false)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${SRC_DIR} OUTPUT_VARIABLE _count
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _count MATCHES "^[0-9]+$")
        set(FITZEL_VER_BUILD ${_count})

        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${SRC_DIR} OUTPUT_VARIABLE FITZEL_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

        # `diff-index --quiet` exits 1 when the working tree differs from HEAD.
        # It needs an up-to-date stat cache to avoid false positives, hence the
        # refresh first (which is why this is not just a --quiet call).
        execute_process(COMMAND ${GIT_EXECUTABLE} update-index -q --refresh
            WORKING_DIRECTORY ${SRC_DIR} ERROR_QUIET OUTPUT_QUIET)
        execute_process(COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
            WORKING_DIRECTORY ${SRC_DIR} RESULT_VARIABLE _dirty
            ERROR_QUIET OUTPUT_QUIET)
        if(NOT _dirty EQUAL 0)
            set(FITZEL_GIT_DIRTY true)
        endif()
    endif()
endif()

set(FITZEL_VERSION
    "${FITZEL_VER_MAJOR}.${FITZEL_VER_MINOR}.${FITZEL_VER_PATCH}.${FITZEL_VER_BUILD}")

set(FITZEL_VERSION_FULL "${FITZEL_VERSION}")
if(FITZEL_GIT_HASH)
    if(FITZEL_GIT_DIRTY)
        set(FITZEL_VERSION_FULL "${FITZEL_VERSION} (${FITZEL_GIT_HASH}, dirty)")
    else()
        set(FITZEL_VERSION_FULL "${FITZEL_VERSION} (${FITZEL_GIT_HASH})")
    endif()
endif()

configure_file(${IN_FILE} ${OUT_FILE} @ONLY)
