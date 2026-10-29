# Only the selected game is generated and compiled. Python is a build dependency.
set(RECOMP_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${RECOMP_GAME}")
option(RECOMP_EMBED_ROM "Embed ROM content with verified CPU instructions erased" OFF)
set(RECOMP_OPT_LEVEL "2" CACHE STRING "Optimization level for release game code")
if(NOT RECOMP_OPT_LEVEL MATCHES "^(2|3)$")
    message(FATAL_ERROR "RECOMP_OPT_LEVEL must be 2 or 3")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
    set(Python3_EXECUTABLE "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe" CACHE FILEPATH "Python with rabbitizer")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/.venv/bin/python")
    set(Python3_EXECUTABLE "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with rabbitizer")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(RECOMP_GENERATED "${CMAKE_BINARY_DIR}/generated/${RECOMP_GAME}")
file(GLOB_RECURSE RECOMP_GENERATOR_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/*.py" "${CMAKE_SOURCE_DIR}/tools/*.toml" "${CMAKE_SOURCE_DIR}/tools/symbols.json"
    "${RECOMP_GAME_DIR}/*.toml" "${RECOMP_GAME_DIR}/*.json" "${RECOMP_GAME_DIR}/*.py")
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
if(RECOMP_EMBED_ROM)
    # Keep packaging separate from translation: native sources always use the
    # pinned original ROM, never the sanitized cartridge image.
    set(RECOMP_PACKAGE_DIR "${RECOMP_GENERATED}/assets")
    set(RECOMP_PACKAGE_COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/package_rom.py"
        --game "${RECOMP_GAME}" --metadata "${RECOMP_GENERATED}/symbols.json"
        --output "${RECOMP_PACKAGE_DIR}")
    execute_process(COMMAND ${RECOMP_PACKAGE_COMMAND} RESULT_VARIABLE RECOMP_PACKAGE_RESULT)
    if(NOT RECOMP_PACKAGE_RESULT EQUAL 0)
        message(FATAL_ERROR "ROM content packaging failed; see the diagnostic above.")
    endif()
    add_custom_command(OUTPUT "${RECOMP_PACKAGE_DIR}/embedded_rom.c"
        "${RECOMP_PACKAGE_DIR}/rom-assets.bin" "${RECOMP_PACKAGE_DIR}/rom-assets.json"
        COMMAND ${RECOMP_PACKAGE_COMMAND}
        DEPENDS ${RECOMP_GENERATOR_INPUTS} "${RECOMP_ROM}" "${RECOMP_GENERATED}/generation.key"
            "${RECOMP_GENERATED}/symbols.json"
        COMMENT "Embedding ${RECOMP_GAME} ROM content without verified CPU instructions" VERBATIM)
endif()
file(GLOB RECOMP_GENERATED_HEADERS "${RECOMP_GENERATED}/*.h")
list(FILTER RECOMP_GENERATED_HEADERS EXCLUDE REGEX "/game_defaults\\.h$")
add_custom_command(OUTPUT ${RECOMP_CPU_SOURCES} ${RECOMP_GENERATED_HEADERS}
    "${RECOMP_GENERATED}/sources.cmake" "${RECOMP_GENERATED}/generation.key"
    "${RECOMP_GENERATED}/symbols.json" "${RECOMP_GENERATED}/coverage.json"
    "${RECOMP_GENERATED}/function-candidates.json"
    COMMAND "${Python3_EXECUTABLE}" -u "${CMAKE_SOURCE_DIR}/tools/generate_game.py"
        --game "${RECOMP_GAME}" --output "${RECOMP_GENERATED}"
    DEPENDS ${RECOMP_GENERATOR_INPUTS} "${RECOMP_ROM}"
    COMMENT "Checking ${RECOMP_GAME} CPU source generation" USES_TERMINAL VERBATIM)
set(RECOMP_LOCAL "${CMAKE_SOURCE_DIR}/local")
configure_file("${CMAKE_SOURCE_DIR}/cmake/game_defaults.h.in" "${RECOMP_GENERATED}/game_defaults.h" @ONLY)
set(RECOMP_CONFIG_DIR "release")
set(RECOMP_CONFIG_SOURCES ${RECOMP_GAME_RELEASE_SOURCES})
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(RECOMP_CONFIG_DIR "debug")
    set(RECOMP_CONFIG_SOURCES ${RECOMP_GAME_DEBUG_SOURCES})
endif()
set(RECOMP_APP "${RECOMP_CONFIG_DIR}/app.c")
add_executable(${RECOMP_GAME} "${RECOMP_GAME_DIR}/${RECOMP_APP}" ${RECOMP_CPU_SOURCES}
    "${CMAKE_SOURCE_DIR}/runtime/game.c" ${RECOMP_GAME_SOURCES} ${RECOMP_CONFIG_SOURCES})
file(GLOB RECOMP_GAME_BROWSE CONFIGURE_DEPENDS "${RECOMP_GAME_DIR}/*.h"
    "${RECOMP_GAME_DIR}/*.toml" "${RECOMP_GAME_DIR}/*.py"
    "${RECOMP_GAME_DIR}/${RECOMP_CONFIG_DIR}/*.h")
target_sources(${RECOMP_GAME} PRIVATE ${RECOMP_GAME_BROWSE} ${RECOMP_GENERATED_HEADERS}
    "${RECOMP_GENERATED}/game_defaults.h" "${CMAKE_SOURCE_DIR}/include/game.h")
set_target_properties(${RECOMP_GAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(RECOMP_EMBED_ROM)
    target_sources(${RECOMP_GAME} PRIVATE "${RECOMP_PACKAGE_DIR}/embedded_rom.c")
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_EMBED_ROM)
    if(NOT MSVC)
        set_source_files_properties("${RECOMP_PACKAGE_DIR}/embedded_rom.c" PROPERTIES COMPILE_OPTIONS "-g0")
    endif()
endif()
target_include_directories(${RECOMP_GAME} PRIVATE "${RECOMP_GENERATED}"
    "${RECOMP_GAME_DIR}" "${RECOMP_GAME_DIR}/${RECOMP_CONFIG_DIR}")
target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_WITH_RT64 RECOMP_WITH_AUDIO)
if(MSVC)
    # Translated MIPS intentionally narrows register values and retains labels
    # and unreachable delay-slot tails. Keep warnings enabled for handwritten code.
    set_source_files_properties(${RECOMP_CPU_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4245;/wd4310;/wd4702")
    target_compile_options(${RECOMP_GAME} PRIVATE /W4 /wd4102 /fp:strict /bigobj
        "$<$<NOT:$<CONFIG:Debug>>:/O2>")
    target_link_options(${RECOMP_GAME} PRIVATE /DEBUG /STACK:8388608
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF>")
else()
    target_compile_options(${RECOMP_GAME} PRIVATE
        "$<$<CONFIG:Debug>:-O0;-g3>" "$<$<NOT:$<CONFIG:Debug>>:-O${RECOMP_OPT_LEVEL};-g1>"
        -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-unused-label
        -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(${RECOMP_GAME} PRIVATE -rdynamic)
endif()
target_link_libraries(${RECOMP_GAME} PRIVATE recomp_renderer recomp_audio)
if(WIN32)
    add_custom_command(TARGET ${RECOMP_GAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:${RECOMP_GAME}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/licenses"
            "$<TARGET_FILE_DIR:${RECOMP_GAME}>/licenses" VERBATIM)
    foreach(dependency openssl sdl2 curl pthreads zlib)
        set(notice "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright")
        if(EXISTS "${notice}")
            add_custom_command(TARGET ${RECOMP_GAME} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${notice}"
                    "$<TARGET_FILE_DIR:${RECOMP_GAME}>/licenses/${dependency}.txt" VERBATIM)
        endif()
    endforeach()
    # Shader compiler DLLs are delay-loaded by RT64. vcpkg deploys its own DLLs.
    foreach(dll dxcompiler dxil)
        add_custom_command(TARGET ${RECOMP_GAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${RECOMP_RT64_SOURCE}/src/contrib/dxc/bin/x64/${dll}.dll"
                "$<TARGET_FILE_DIR:${RECOMP_GAME}>" VERBATIM)
    endforeach()
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/debug-output/${RECOMP_GAME}")
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_DEBUG)
    target_sources(${RECOMP_GAME} PRIVATE "${CMAKE_SOURCE_DIR}/runtime/debug_game.c")
endif()
if(RECOMP_GAME STREQUAL "goldeneye-us")
    target_include_directories(${RECOMP_GAME} SYSTEM PRIVATE ${RECOMP_RT64_INCLUDES})
    target_compile_definitions(${RECOMP_GAME} PRIVATE HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
endif()
recomp_source_groups(${RECOMP_GAME})
add_custom_target(run COMMAND $<TARGET_FILE:${RECOMP_GAME}> ${RECOMP_RUN_ARGS}
    DEPENDS ${RECOMP_GAME} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL)
add_custom_target(pack
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=$<TARGET_FILE:${RECOMP_GAME}>"
        "-DOUTPUT=$<TARGET_FILE_DIR:${RECOMP_GAME}>/packed/$<TARGET_FILE_NAME:${RECOMP_GAME}>"
        "-DUPX_EXECUTABLE=${RECOMP_UPX_EXECUTABLE}"
        -P "${CMAKE_SOURCE_DIR}/cmake/PackUPX.cmake"
    DEPENDS ${RECOMP_GAME}
    COMMENT "Packing ${RECOMP_GAME} with maximum UPX compression"
    USES_TERMINAL VERBATIM)
add_custom_target(report COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/generate_game.py"
    --game "${RECOMP_GAME}" --output "${CMAKE_BINARY_DIR}/reports/${RECOMP_GAME}" --report-only
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL)
