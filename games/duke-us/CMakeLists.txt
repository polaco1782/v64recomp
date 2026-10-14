set(RECOMP_DUKE_GENERATED "${CMAKE_SOURCE_DIR}/build/duke-us/rom/boot"
    CACHE PATH "Generated Duke Nukem 64 CPU C directory")
if(EXISTS "${RECOMP_DUKE_GENERATED}/functions.c")
    add_executable(duke_native app.c "${RECOMP_DUKE_GENERATED}/functions.c")
    set_target_properties(duke_native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(duke_native PRIVATE "${RECOMP_DUKE_GENERATED}")
    target_compile_options(duke_native PRIVATE ${RECOMP_GAME_OPTIMIZATION} -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(duke_native PRIVATE -rdynamic)
    target_link_libraries(duke_native PRIVATE recomp_runtime)
endif()

set(RECOMP_DUKE_RENDER_GENERATED "${CMAKE_SOURCE_DIR}/build/duke-us/rom/startup"
    CACHE PATH "Generated Duke Nukem 64 rendered CPU C directory")
if(RECOMP_BUILD_RT64 AND RECOMP_BUILD_AUDIO
        AND EXISTS "${RECOMP_DUKE_RENDER_GENERATED}/functions.c")
    add_executable(duke_native_rt64 app.c "${RECOMP_DUKE_RENDER_GENERATED}/functions.c")
    set_target_properties(duke_native_rt64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(duke_native_rt64 PRIVATE "${RECOMP_DUKE_RENDER_GENERATED}")
    target_compile_definitions(duke_native_rt64 PRIVATE RECOMP_WITH_RT64)
    target_compile_options(duke_native_rt64 PRIVATE ${RECOMP_GAME_OPTIMIZATION} -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(duke_native_rt64 PRIVATE -rdynamic)
    target_link_libraries(duke_native_rt64 PRIVATE recomp_renderer recomp_audio)
endif()
