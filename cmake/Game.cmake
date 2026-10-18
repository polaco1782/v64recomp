# Only the selected game is generated and compiled. Python is a build dependency.
set(RECOMP_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${RECOMP_GAME}")
set(RECOMP_OPT_LEVEL "2" CACHE STRING "Optimization level for release game code")
if(NOT RECOMP_OPT_LEVEL MATCHES "^(2|3)$")
    message(FATAL_ERROR "RECOMP_OPT_LEVEL must be 2 or 3")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/.venv/bin/python")
    set(Python3_EXECUTABLE "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with rabbitizer")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(RECOMP_GENERATED "${CMAKE_BINARY_DIR}/generated/${RECOMP_GAME}")
file(GLOB_RECURSE RECOMP_GENERATOR_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/*.py" "${CMAKE_SOURCE_DIR}/tools/*.toml"
    "${RECOMP_GAME_DIR}/*.toml" "${RECOMP_GAME_DIR}/*.json" "${RECOMP_GAME_DIR}/release/*.py")
# Generate during configure to discover the exact set of split CPU sources.
# The content cache makes repeat configures cheap and detects deleted outputs.
message(STATUS "Checking ${RECOMP_GAME} CPU source generation")
execute_process(COMMAND "${Python3_EXECUTABLE}" -u "${CMAKE_SOURCE_DIR}/tools/generate_game.py"
    --game "${RECOMP_GAME}" --output "${RECOMP_GENERATED}"
    OUTPUT_VARIABLE RECOMP_GENERATION_LOG
    ERROR_VARIABLE RECOMP_GENERATION_LOG
    ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE
    RESULT_VARIABLE RECOMP_GENERATION_RESULT)
file(WRITE "${CMAKE_BINARY_DIR}/generation.log" "${RECOMP_GENERATION_LOG}")
if(NOT RECOMP_GENERATION_RESULT EQUAL 0)
    message(FATAL_ERROR "ROM source generation failed. Import the game's pinned ROM into local/; see README.md.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${RECOMP_GENERATOR_INPUTS} "${RECOMP_GENERATED}/sources.cmake" "${RECOMP_ROM}")
include("${RECOMP_GENERATED}/sources.cmake")
file(GLOB RECOMP_GENERATED_HEADERS "${RECOMP_GENERATED}/*.h")
list(FILTER RECOMP_GENERATED_HEADERS EXCLUDE REGEX "/game_defaults\\.h$")
add_custom_command(OUTPUT ${RECOMP_CPU_SOURCES} ${RECOMP_GENERATED_HEADERS}
    "${RECOMP_GENERATED}/sources.cmake" "${RECOMP_GENERATED}/generation.key"
    COMMAND "${Python3_EXECUTABLE}" -u "${CMAKE_SOURCE_DIR}/tools/generate_game.py"
        --game "${RECOMP_GAME}" --output "${RECOMP_GENERATED}"
    DEPENDS ${RECOMP_GENERATOR_INPUTS} "${RECOMP_ROM}"
    COMMENT "Checking ${RECOMP_GAME} CPU source generation" USES_TERMINAL VERBATIM)
set(RECOMP_LOCAL "${CMAKE_SOURCE_DIR}/local")
configure_file("${CMAKE_SOURCE_DIR}/cmake/game_defaults.h.in" "${RECOMP_GENERATED}/game_defaults.h" @ONLY)
set(RECOMP_APP "release/app.c")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(RECOMP_APP "debug/app.c")
endif()
add_executable(${RECOMP_GAME} "${RECOMP_GAME_DIR}/${RECOMP_APP}" ${RECOMP_CPU_SOURCES}
    "${CMAKE_SOURCE_DIR}/runtime/game.c" ${RECOMP_GAME_SOURCES})
set_target_properties(${RECOMP_GAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_include_directories(${RECOMP_GAME} PRIVATE "${RECOMP_GENERATED}" "${RECOMP_GAME_DIR}/release")
target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_WITH_RT64 RECOMP_WITH_AUDIO)
target_compile_options(${RECOMP_GAME} PRIVATE
    "$<$<CONFIG:Debug>:-O0;-g3>" "$<$<NOT:$<CONFIG:Debug>>:-O${RECOMP_OPT_LEVEL};-g1>"
    -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-unused-label
    -fno-fast-math -frounding-math -ffp-contract=off)
target_link_options(${RECOMP_GAME} PRIVATE -rdynamic)
target_link_libraries(${RECOMP_GAME} PRIVATE recomp_renderer recomp_audio)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/debug-output/${RECOMP_GAME}")
    target_include_directories(${RECOMP_GAME} PRIVATE "${RECOMP_GAME_DIR}/debug")
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_DEBUG)
    target_sources(${RECOMP_GAME} PRIVATE "${CMAKE_SOURCE_DIR}/runtime/debug_game.c")
endif()
if(RECOMP_GAME STREQUAL "goldeneye-us")
    target_include_directories(${RECOMP_GAME} SYSTEM PRIVATE ${RECOMP_RT64_INCLUDES})
    target_compile_definitions(${RECOMP_GAME} PRIVATE HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
endif()
add_custom_target(run COMMAND $<TARGET_FILE:${RECOMP_GAME}> ${RECOMP_RUN_ARGS}
    DEPENDS ${RECOMP_GAME} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL)
add_custom_target(report COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/generate_game.py"
    --game "${RECOMP_GAME}" --output "${CMAKE_BINARY_DIR}/reports/${RECOMP_GAME}" --report-only
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL)
