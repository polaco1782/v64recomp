set(RECOMP_OOT_GENERATED "${CMAKE_SOURCE_DIR}/build/oot-us-v12/rom/boot"
    CACHE PATH "Generated Ocarina of Time CPU C directory")
if(EXISTS "${RECOMP_OOT_GENERATED}/functions.c")
    add_executable(oot_native app.c "${RECOMP_OOT_GENERATED}/functions.c")
    set_target_properties(oot_native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(oot_native PRIVATE "${RECOMP_OOT_GENERATED}")
    target_compile_options(oot_native PRIVATE ${RECOMP_GAME_OPTIMIZATION} -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(oot_native PRIVATE -rdynamic)
    target_link_libraries(oot_native PRIVATE recomp_runtime)
endif()

set(RECOMP_OOT_RENDER_GENERATED "${CMAKE_SOURCE_DIR}/build/oot-us-v12/rom/startup"
    CACHE PATH "Generated Ocarina of Time rendered CPU C directory")
if(RECOMP_BUILD_RT64 AND RECOMP_BUILD_AUDIO
        AND EXISTS "${RECOMP_OOT_RENDER_GENERATED}/functions.c")
    file(GLOB OOT_CPU_SOURCES "${RECOMP_OOT_RENDER_GENERATED}/cpu-*.c")
    add_executable(oot_native_rt64 app.c "${RECOMP_OOT_RENDER_GENERATED}/functions.c" ${OOT_CPU_SOURCES})
    set_target_properties(oot_native_rt64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(oot_native_rt64 PRIVATE "${RECOMP_OOT_RENDER_GENERATED}")
    target_compile_definitions(oot_native_rt64 PRIVATE RECOMP_WITH_RT64)
    target_compile_options(oot_native_rt64 PRIVATE ${RECOMP_GAME_OPTIMIZATION} -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(oot_native_rt64 PRIVATE -rdynamic)
    target_link_libraries(oot_native_rt64 PRIVATE recomp_renderer recomp_audio)
endif()
