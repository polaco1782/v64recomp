set(RECOMP_NIGHTMARE_GENERATED "${CMAKE_SOURCE_DIR}/build/nightmare-us/rom/boot"
    CACHE PATH "Generated Nightmare Creatures CPU C directory")
if(EXISTS "${RECOMP_NIGHTMARE_GENERATED}/functions.c")
    add_executable(nightmare_native app.c "${RECOMP_NIGHTMARE_GENERATED}/functions.c")
    set_target_properties(nightmare_native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(nightmare_native PRIVATE "${RECOMP_NIGHTMARE_GENERATED}")
    target_compile_options(nightmare_native PRIVATE ${RECOMP_GAME_OPTIMIZATION} -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(nightmare_native PRIVATE -rdynamic)
    target_link_libraries(nightmare_native PRIVATE recomp_runtime)
endif()

set(RECOMP_NIGHTMARE_RENDER_GENERATED "${CMAKE_SOURCE_DIR}/build/nightmare-us/rom/startup"
    CACHE PATH "Generated Nightmare Creatures rendered CPU C directory")
if(RECOMP_BUILD_RT64 AND RECOMP_BUILD_AUDIO
        AND EXISTS "${RECOMP_NIGHTMARE_RENDER_GENERATED}/functions.c")
    add_executable(nightmare_native_rt64 app.c "${RECOMP_NIGHTMARE_RENDER_GENERATED}/functions.c")
    set_target_properties(nightmare_native_rt64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(nightmare_native_rt64 PRIVATE "${RECOMP_NIGHTMARE_RENDER_GENERATED}")
    target_compile_definitions(nightmare_native_rt64 PRIVATE RECOMP_WITH_RT64)
    target_compile_options(nightmare_native_rt64 PRIVATE ${RECOMP_GAME_OPTIMIZATION} -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(nightmare_native_rt64 PRIVATE -rdynamic)
    target_link_libraries(nightmare_native_rt64 PRIVATE recomp_renderer recomp_audio)
endif()
