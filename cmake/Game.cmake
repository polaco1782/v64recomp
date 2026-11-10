# Only the selected game is generated and compiled. Python is a build dependency.
if(RECOMP_RENDERER STREQUAL "gles2" AND NOT RECOMP_GAME_GLES2_SUPPORTED)
    message(FATAL_ERROR "${RECOMP_GAME} has no checked GLES2 graphics adapter")
endif()
set(RECOMP_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${RECOMP_GAME}")
if(NOT DEFINED RECOMP_OPTIMIZE_DEBUG_DEFAULT)
    set(RECOMP_OPTIMIZE_DEBUG_DEFAULT OFF)
endif()
option(RECOMP_OPTIMIZE_DEBUG "Optimize Debug game/runtime code while retaining diagnostics and symbols"
    ${RECOMP_OPTIMIZE_DEBUG_DEFAULT})
option(RECOMP_EMBED_ROM "Embed verified ROM content in the game executable" OFF)
option(RECOMP_STRIP_EMBEDDED_ROM "Erase verified CPU instructions from embedded ROM content" ON)
set(RECOMP_EMBED_TEXTURE_PACK "" CACHE FILEPATH "Texture ZIP to append to the game executable (empty disables embedding)")
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
if(RECOMP_EMBED_TEXTURE_PACK)
    get_filename_component(RECOMP_EMBED_TEXTURE_PACK "${RECOMP_EMBED_TEXTURE_PACK}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_texture_pack.py"
        --pack "${RECOMP_EMBED_TEXTURE_PACK}" RESULT_VARIABLE RECOMP_TEXTURE_PACK_RESULT)
    if(NOT RECOMP_TEXTURE_PACK_RESULT EQUAL 0)
        message(FATAL_ERROR "Invalid RECOMP_EMBED_TEXTURE_PACK; supply a supported texture ZIP.")
    endif()
endif()
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
    if(NOT RECOMP_STRIP_EMBEDDED_ROM)
        list(APPEND RECOMP_PACKAGE_COMMAND --keep-cpu-code)
    endif()
    execute_process(COMMAND ${RECOMP_PACKAGE_COMMAND} RESULT_VARIABLE RECOMP_PACKAGE_RESULT)
    if(NOT RECOMP_PACKAGE_RESULT EQUAL 0)
        message(FATAL_ERROR "ROM content packaging failed; see the diagnostic above.")
    endif()
    add_custom_command(OUTPUT "${RECOMP_PACKAGE_DIR}/embedded_rom.c"
        "${RECOMP_PACKAGE_DIR}/rom-assets.bin" "${RECOMP_PACKAGE_DIR}/rom-assets.json"
        COMMAND ${RECOMP_PACKAGE_COMMAND}
        DEPENDS ${RECOMP_GENERATOR_INPUTS} "${RECOMP_ROM}" "${RECOMP_GENERATED}/generation.key"
            "${RECOMP_GENERATED}/symbols.json"
        COMMENT "Embedding ${RECOMP_GAME} ROM content" VERBATIM)
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
add_executable(${RECOMP_GAME} "${RECOMP_GAME_DIR}/main.c" ${RECOMP_CPU_SOURCES}
    "${CMAKE_SOURCE_DIR}/runtime/game.c" ${RECOMP_GAME_SOURCES})
file(GLOB RECOMP_GAME_BROWSE CONFIGURE_DEPENDS "${RECOMP_GAME_DIR}/*.h"
    "${RECOMP_GAME_DIR}/*.toml" "${RECOMP_GAME_DIR}/*.py")
target_sources(${RECOMP_GAME} PRIVATE ${RECOMP_GAME_BROWSE} ${RECOMP_GENERATED_HEADERS}
    "${RECOMP_GENERATED}/game_defaults.h" "${CMAKE_SOURCE_DIR}/include/game.h")
set_target_properties(${RECOMP_GAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# PIC previously enabled -fPIE for CPU objects without enabling PIE at link
# time. Check both drivers because games can include C++ graphics adapters.
include(CheckPIESupported)
set(RECOMP_PIE_LANGUAGES C)
if(CMAKE_CXX_COMPILER_LOADED)
    list(APPEND RECOMP_PIE_LANGUAGES CXX)
endif()
check_pie_supported(LANGUAGES ${RECOMP_PIE_LANGUAGES} OUTPUT_VARIABLE RECOMP_PIE_ERROR)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND (NOT CMAKE_C_LINK_PIE_SUPPORTED
            OR (CMAKE_CXX_COMPILER_LOADED AND NOT CMAKE_CXX_LINK_PIE_SUPPORTED)))
    message(FATAL_ERROR "The selected toolchain cannot link PIE games: ${RECOMP_PIE_ERROR}")
endif()
set_property(TARGET ${RECOMP_GAME} PROPERTY POSITION_INDEPENDENT_CODE TRUE)
recomp_separate_release_debug(${RECOMP_GAME})
if(RECOMP_EMBED_TEXTURE_PACK)
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_EMBED_TEXTURE_PACK)
    # A changed ZIP must relink before appending, avoiding accumulated overlays.
    # A generated source also tracks this dependency with Visual Studio generators.
    set(RECOMP_TEXTURE_PACK_SOURCE "${RECOMP_GENERATED}/assets/embedded_texture_pack.c")
    add_custom_command(OUTPUT "${RECOMP_TEXTURE_PACK_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_texture_pack.py"
            --pack "${RECOMP_EMBED_TEXTURE_PACK}" --build-source "${RECOMP_TEXTURE_PACK_SOURCE}"
        DEPENDS "${RECOMP_EMBED_TEXTURE_PACK}" "${CMAKE_SOURCE_DIR}/tools/embed_texture_pack.py"
        COMMENT "Checking embedded texture ZIP" VERBATIM)
    target_sources(${RECOMP_GAME} PRIVATE "${RECOMP_TEXTURE_PACK_SOURCE}")
    add_custom_command(TARGET ${RECOMP_GAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/embed_texture_pack.py"
            --binary "$<TARGET_FILE:${RECOMP_GAME}>" --pack "${RECOMP_EMBED_TEXTURE_PACK}"
        COMMENT "Appending ${RECOMP_GAME} texture ZIP" VERBATIM)
endif()
if(RECOMP_EMBED_ROM)
    target_sources(${RECOMP_GAME} PRIVATE "${RECOMP_PACKAGE_DIR}/embedded_rom.c")
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_EMBED_ROM)
    if(NOT MSVC)
        set_source_files_properties("${RECOMP_PACKAGE_DIR}/embedded_rom.c" PROPERTIES COMPILE_OPTIONS "-g0")
    endif()
endif()
target_include_directories(${RECOMP_GAME} PRIVATE "${RECOMP_GENERATED}" "${RECOMP_GAME_DIR}")
target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_WITH_AUDIO)
if(RECOMP_RENDERER STREQUAL "gles2")
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_WITH_GLES2)
else()
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_WITH_RT64)
endif()
if(MSVC)
    # Translated MIPS intentionally narrows register values and retains labels
    # and unreachable delay-slot tails. Keep warnings enabled for handwritten code.
    set_source_files_properties(${RECOMP_CPU_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4245;/wd4310;/wd4702")
    target_compile_options(${RECOMP_GAME} PRIVATE /W4 /wd4102 /fp:strict /bigobj
        "$<$<NOT:$<CONFIG:Debug>>:/O2>")
    target_link_options(${RECOMP_GAME} PRIVATE /DEBUG /STACK:8388608
        "$<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF>")
else()
    target_compile_options(${RECOMP_GAME} PRIVATE
        "$<$<CONFIG:Debug>:-O0;-g3>" "$<$<NOT:$<CONFIG:Debug>>:-O${RECOMP_OPT_LEVEL};-g1>"
        -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-unused-label
        -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(${RECOMP_GAME} PRIVATE -rdynamic)
endif()
recomp_optimize_release_target(${RECOMP_GAME})
recomp_optimize_release_cpu(${RECOMP_CPU_SOURCES})
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
    if(RECOMP_RENDERER STREQUAL "rt64")
        foreach(dll dxcompiler dxil)
            add_custom_command(TARGET ${RECOMP_GAME} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${RECOMP_RT64_SOURCE}/src/contrib/dxc/bin/x64/${dll}.dll"
                    "$<TARGET_FILE_DIR:${RECOMP_GAME}>" VERBATIM)
        endforeach()
    endif()
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/debug-output/${RECOMP_GAME}")
    target_compile_definitions(${RECOMP_GAME} PRIVATE RECOMP_DEBUG)
endif()
if(RECOMP_GAME STREQUAL "goldeneye-us")
    target_include_directories(${RECOMP_GAME} SYSTEM PRIVATE ${RECOMP_RT64_INCLUDES})
    target_compile_definitions(${RECOMP_GAME} PRIVATE HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND RECOMP_OPTIMIZE_DEBUG)
    include("${CMAKE_SOURCE_DIR}/cmake/OptimizeDebug.cmake")
endif()
recomp_source_groups(${RECOMP_GAME})
add_custom_target(run COMMAND $<TARGET_FILE:${RECOMP_GAME}> ${RECOMP_RUN_ARGS}
    DEPENDS ${RECOMP_GAME} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL)
add_custom_target(pack
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=$<TARGET_FILE:${RECOMP_GAME}>"
        "-DOUTPUT=$<TARGET_FILE_DIR:${RECOMP_GAME}>/packed/$<TARGET_FILE_NAME:${RECOMP_GAME}>"
        "-DUPX_EXECUTABLE=${RECOMP_UPX_EXECUTABLE}"
        "-DTEXTURE_PACK=${RECOMP_EMBED_TEXTURE_PACK}"
        "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
        -P "${CMAKE_SOURCE_DIR}/cmake/PackUPX.cmake"
    DEPENDS ${RECOMP_GAME}
    COMMENT "Packing ${RECOMP_GAME} with maximum UPX compression"
    USES_TERMINAL VERBATIM)
add_custom_target(report COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/generate_game.py"
    --game "${RECOMP_GAME}" --output "${CMAKE_BINARY_DIR}/reports/${RECOMP_GAME}" --report-only
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL)
add_custom_target(candidate-review
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/review_candidates.py"
        --game "${RECOMP_GAME}" --build-dir "${CMAKE_BINARY_DIR}"
    DEPENDS "${RECOMP_GENERATED}/function-candidates.json"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Ranking ${RECOMP_GAME} missing-function candidates"
    USES_TERMINAL VERBATIM)
add_custom_target(candidate-review-roots
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/review_candidates.py"
        --game "${RECOMP_GAME}" --build-dir "${CMAKE_BINARY_DIR}"
        --roots-only --codex --max-groups 3
    DEPENDS "${RECOMP_GENERATED}/function-candidates.json"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Reviewing ${RECOMP_GAME} candidate call trees and saving checked roots"
    USES_TERMINAL VERBATIM)
add_custom_target(candidate-review-tables
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/review_candidates.py"
        --game "${RECOMP_GAME}" --build-dir "${CMAKE_BINARY_DIR}"
        --codex --max-groups 3
    DEPENDS "${RECOMP_GENERATED}/function-candidates.json"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Reviewing ${RECOMP_GAME} indirect-call tables and saving checked entries"
    USES_TERMINAL VERBATIM)
add_custom_target(promote-live-patches
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/promote_live_patches.py"
        --game "${RECOMP_GAME}" --build-dir "${CMAKE_BINARY_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/review_discovery.py"
        --game "${RECOMP_GAME}" --build-dir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Promoting live functions and reviewing candidate roots for ${RECOMP_GAME}"
    USES_TERMINAL VERBATIM)
