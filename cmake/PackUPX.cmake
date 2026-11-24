cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}" OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "Packing requires an existing INPUT executable and an OUTPUT path.")
endif()
if(NOT UPX_EXECUTABLE)
    unset(UPX_EXECUTABLE)
    find_program(UPX_EXECUTABLE NAMES upx upx-ucl)
endif()
if(NOT UPX_EXECUTABLE)
    message(FATAL_ERROR "UPX was not found. Install upx or configure -DRECOMP_UPX_EXECUTABLE=/path/to/upx.")
endif()

get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
# Always start from the linked executable so repeated pack commands work.
file(REMOVE "${OUTPUT}")
execute_process(COMMAND "${UPX_EXECUTABLE}" --best --ultra-brute -o "${OUTPUT}" "${INPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${UPX_EXECUTABLE}" --test "${OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Packed executable: ${OUTPUT}")
