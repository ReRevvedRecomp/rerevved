# Prints the version an SDK checkout derives for its HEAD, using that
# checkout's own cmake/rex_version.cmake rather than a reimplementation.
#
#   cmake -DSOURCE_DIR=<sdk checkout> -DFLOOR_MAJOR=<n> -DFLOOR_MINOR=<n>
#         -P scripts/sdk_version.cmake
cmake_minimum_required(VERSION 3.20)

foreach(name SOURCE_DIR FLOOR_MAJOR FLOOR_MINOR)
    if(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
        message(FATAL_ERROR "sdk_version.cmake: -D${name}=<value> is required")
    endif()
endforeach()

include("${SOURCE_DIR}/cmake/rex_version.cmake")
rex_resolve_version(sdk_version
    FLOOR_MAJOR ${FLOOR_MAJOR}
    FLOOR_MINOR ${FLOOR_MINOR}
    SOURCE_DIR "${SOURCE_DIR}")
# message() writes to stderr; echo keeps the result alone on stdout.
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${sdk_version}")
