set(RECOMP_QUAKE_GENERATED "${CMAKE_SOURCE_DIR}/build/quake-us/rom/boot"
    CACHE PATH "Generated Quake 64 CPU C directory")
if(EXISTS "${RECOMP_QUAKE_GENERATED}/functions.c")
    add_executable(quake_native app.c "${RECOMP_QUAKE_GENERATED}/functions.c")
    set_target_properties(quake_native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(quake_native PRIVATE "${RECOMP_QUAKE_GENERATED}")
    target_compile_options(quake_native PRIVATE -O2 -g1 -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(quake_native PRIVATE -rdynamic)
    target_link_libraries(quake_native PRIVATE recomp_runtime)
endif()

set(RECOMP_QUAKE_RENDER_GENERATED "${CMAKE_SOURCE_DIR}/build/quake-us/rom/startup"
    CACHE PATH "Generated Quake 64 rendered CPU C directory")
if(RECOMP_BUILD_RT64 AND RECOMP_BUILD_AUDIO
        AND EXISTS "${RECOMP_QUAKE_RENDER_GENERATED}/functions.c")
    add_executable(quake_native_rt64 app.c "${RECOMP_QUAKE_RENDER_GENERATED}/functions.c")
    set_target_properties(quake_native_rt64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(quake_native_rt64 PRIVATE "${RECOMP_QUAKE_RENDER_GENERATED}")
    target_compile_definitions(quake_native_rt64 PRIVATE RECOMP_WITH_RT64)
    target_compile_options(quake_native_rt64 PRIVATE -O2 -g1 -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(quake_native_rt64 PRIVATE -rdynamic)
    target_link_libraries(quake_native_rt64 PRIVATE recomp_renderer recomp_audio)
endif()
