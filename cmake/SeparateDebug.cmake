cmake_minimum_required(VERSION 3.20)

foreach(argument INPUT DEBUG_OUTPUT OBJCOPY STRIP)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "Separating debug information requires ${argument}")
    endif()
endforeach()
if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Release executable does not exist: ${INPUT}")
endif()

# The caller supplies a freshly linked ELF, before any texture ZIP is appended.
# Keep the linked executable intact until both outputs have been prepared.
execute_process(COMMAND "${OBJCOPY}" --only-keep-debug --compress-debug-sections=zlib
    "${INPUT}" "${DEBUG_OUTPUT}.tmp"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${STRIP}" --strip-debug -o "${INPUT}.stripped" "${INPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
file(RENAME "${DEBUG_OUTPUT}.tmp" "${DEBUG_OUTPUT}")
execute_process(COMMAND "${OBJCOPY}" "--add-gnu-debuglink=${DEBUG_OUTPUT}" "${INPUT}.stripped"
    COMMAND_ERROR_IS_FATAL ANY)
file(RENAME "${INPUT}.stripped" "${INPUT}")
