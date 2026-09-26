# Applies a git patch to a fetched source tree exactly once.
#
# Usage: cmake -DPATCH_FILE=<patch> -DSOURCE_DIR=<dir> -P apply-patch.cmake
#
# FetchContent may run the patch step again on re-configure, so a patch that
# is already present (detected with a reverse dry run) is skipped.

if(NOT PATCH_FILE OR NOT SOURCE_DIR)
    message(FATAL_ERROR "apply-patch.cmake needs PATCH_FILE and SOURCE_DIR")
endif()

find_package(Git REQUIRED QUIET)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --ignore-whitespace "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)
if(already_applied EQUAL 0)
    message(STATUS "Patch already applied: ${PATCH_FILE}")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not apply ${PATCH_FILE} in ${SOURCE_DIR}:\n${error_output}")
endif()
message(STATUS "Applied patch: ${PATCH_FILE}")
