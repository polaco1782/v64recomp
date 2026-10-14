set(RECOMP_SHADOWMAN_GENERATED "${CMAKE_SOURCE_DIR}/build/shadowman-us/rom/boot"
    CACHE PATH "Generated Shadow Man CPU C directory")
if(EXISTS "${RECOMP_SHADOWMAN_GENERATED}/functions.c")
    add_executable(shadowman_native app.c pager.c "${RECOMP_SHADOWMAN_GENERATED}/functions.c")
    set_target_properties(shadowman_native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(shadowman_native PRIVATE "${RECOMP_SHADOWMAN_GENERATED}")
    target_compile_options(shadowman_native PRIVATE -O2 -g1 -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(shadowman_native PRIVATE -rdynamic)
    target_link_libraries(shadowman_native PRIVATE recomp_runtime)
endif()

set(RECOMP_SHADOWMAN_RENDER_GENERATED "${CMAKE_SOURCE_DIR}/build/shadowman-us/rom/startup"
    CACHE PATH "Generated Shadow Man rendered CPU C directory")
if(RECOMP_BUILD_RT64 AND RECOMP_BUILD_AUDIO
        AND EXISTS "${RECOMP_SHADOWMAN_RENDER_GENERATED}/functions.c")
    add_executable(shadowman_native_rt64 app.c pager.c "${RECOMP_SHADOWMAN_RENDER_GENERATED}/functions.c")
    set_target_properties(shadowman_native_rt64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(shadowman_native_rt64 PRIVATE "${RECOMP_SHADOWMAN_RENDER_GENERATED}")
    target_compile_definitions(shadowman_native_rt64 PRIVATE RECOMP_WITH_RT64)
    target_compile_options(shadowman_native_rt64 PRIVATE -O2 -g1 -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(shadowman_native_rt64 PRIVATE -rdynamic)
    target_link_libraries(shadowman_native_rt64 PRIVATE recomp_renderer recomp_audio)
endif()
