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
set(OVERLAY_OPTIONS)
if(TEXTURE_PACK)
    # UPX changes executable size, invalidating absolute ZIP offsets. Append
    # the source archive again after compression, with freshly rebased offsets.
    list(APPEND OVERLAY_OPTIONS --overlay=strip)
endif()
execute_process(COMMAND "${UPX_EXECUTABLE}" --best --ultra-brute ${OVERLAY_OPTIONS} -o "${OUTPUT}" "${INPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${UPX_EXECUTABLE}" --test "${OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
if(TEXTURE_PACK)
    execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_texture_pack.py"
        --binary "${OUTPUT}" --pack "${TEXTURE_PACK}" COMMAND_ERROR_IS_FATAL ANY)
endif()
message(STATUS "Packed executable: ${OUTPUT}")
