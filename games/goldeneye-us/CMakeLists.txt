set(RECOMP_GOLDENEYE_GENERATED "${CMAKE_SOURCE_DIR}/build/goldeneye-us/rom/startup"
    CACHE PATH "Generated GoldenEye startup CPU C directory")
if(RECOMP_BUILD_RT64 AND RECOMP_BUILD_AUDIO
        AND EXISTS "${RECOMP_GOLDENEYE_GENERATED}/functions.c")
    add_executable(goldeneye_native_rt64 startup.c pager.c graphics.cpp
        "${RECOMP_GOLDENEYE_GENERATED}/functions.c")
    set_target_properties(goldeneye_native_rt64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(goldeneye_native_rt64 PRIVATE "${RECOMP_GOLDENEYE_GENERATED}")
    target_include_directories(goldeneye_native_rt64 SYSTEM PRIVATE ${RECOMP_RT64_INCLUDES})
    target_compile_definitions(goldeneye_native_rt64 PRIVATE RECOMP_WITH_RT64
        HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    target_compile_options(goldeneye_native_rt64 PRIVATE -O2 -g1 -fno-omit-frame-pointer
        -Wall -Wextra -Werror -Wno-unused-label -fno-fast-math -frounding-math -ffp-contract=off)
    target_link_options(goldeneye_native_rt64 PRIVATE -rdynamic)
    target_link_libraries(goldeneye_native_rt64 PRIVATE recomp_renderer recomp_audio)
endif()
