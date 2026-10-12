# Applies one patch to a fetched dependency, once: run as a FetchContent
# PATCH_COMMAND (cmake -DPATCH=<file> -P ApplyPatch.cmake, in the source dir).
# The step runs again whenever the dependency's declaration changes, so an
# already-applied patch must be a no-op rather than an error.
execute_process(COMMAND git apply --reverse --check "${PATCH}"
                RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    return()
endif()
execute_process(COMMAND git apply --whitespace=nowarn "${PATCH}"
                RESULT_VARIABLE failed)
if(NOT failed EQUAL 0)
    message(FATAL_ERROR "Could not apply ${PATCH}")
endif()
