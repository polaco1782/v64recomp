if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED)
endif()
add_library(recomp_rdp STATIC backends/rdp_triangle.c include/rdp_triangle.h)
target_link_libraries(recomp_rdp PUBLIC recomp_runtime)
if(MSVC)
    target_compile_options(recomp_rdp PRIVATE /W4 /fp:strict)
else()
    target_compile_options(recomp_rdp PRIVATE -Wall -Wextra -Werror -g1 -fno-omit-frame-pointer -fno-fast-math -ffp-contract=off)
endif()
add_library(recomp_rdp_gles2 STATIC
    backends/gles2/interpolation.c
    backends/gles2/rdp.c
    backends/gles2/render_queue.c
    backends/gles2/context.c
    backends/gles2/controls.c
    backends/gles2/framebuffer.c
    backends/gles2/hq2x.c
    backends/gles2/hq2x.h
    backends/gles2/textures.c
    backends/gles2/combiner.c
    backends/gles2/wireframe.c
    backends/gles2/internal.h
    backends/gles2/screen_space.h
    backends/gles2/torch_lights.c
    include/torch_lights.h
    backends/input.c
    include/rdp_gles2.h)
target_link_libraries(recomp_rdp_gles2 PUBLIC recomp_rdp SDL2::SDL2)
target_compile_definitions(recomp_rdp_gles2 PRIVATE SDL_MAIN_HANDLED)

configure_file("licenses/hq2x.txt" "${CMAKE_BINARY_DIR}/bin/licenses/hq2x.txt" COPYONLY)

option(RECOMP_GLES2_TEXTURE_PACKS "Load and edit native GLES texture packs" ON)
set(_imgui "${PROJECT_SOURCE_DIR}/third_party/cimgui/imgui")
option(RECOMP_GLES2_CHEATS "Build the GameShark overlay from the pinned Dear ImGui sources" ON)
if(RECOMP_GLES2_CHEATS)
    enable_language(CXX)
    add_library(recomp_imgui STATIC third_party/cimgui/cimgui.cpp
        "${_imgui}/imgui.cpp" "${_imgui}/imgui_demo.cpp" "${_imgui}/imgui_draw.cpp"
        "${_imgui}/imgui_tables.cpp" "${_imgui}/imgui_widgets.cpp"
        "${_imgui}/backends/imgui_impl_sdl2.cpp" "${_imgui}/backends/imgui_impl_opengl3.cpp")
    target_compile_features(recomp_imgui PRIVATE cxx_std_17)
    target_compile_definitions(recomp_imgui PRIVATE IMGUI_IMPL_OPENGL_ES3 IMGUI_DISABLE_OBSOLETE_FUNCTIONS
        "IMGUI_IMPL_API=extern \"C\"" CIMGUI_NO_EXPORT)
    target_include_directories(recomp_imgui SYSTEM PRIVATE "${_imgui}")
    target_link_libraries(recomp_imgui PRIVATE SDL2::SDL2)
    add_library(recomp_gles2_cheats STATIC backends/cheat_database.c backends/gles2/cheats.c)
    target_compile_definitions(recomp_gles2_cheats PRIVATE SDL_MAIN_HANDLED CIMGUI_NO_EXPORT)
    target_include_directories(recomp_gles2_cheats SYSTEM PRIVATE third_party/cimgui)
    target_link_libraries(recomp_gles2_cheats PRIVATE recomp_runtime recomp_imgui SDL2::SDL2)
    if(NOT EMSCRIPTEN)
        find_package(CURL 7.85 REQUIRED)
        find_path(RECOMP_GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
        find_library(RECOMP_GLES_LIBRARY NAMES GLESv2 libGLESv2 REQUIRED)
        target_include_directories(recomp_imgui SYSTEM PRIVATE "${RECOMP_GLES3_INCLUDE_DIR}")
        target_link_libraries(recomp_imgui PRIVATE "${RECOMP_GLES_LIBRARY}")
        target_link_libraries(recomp_gles2_cheats PRIVATE CURL::libcurl)
    endif()
    configure_file("third_party/cimgui/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/cimgui.txt" COPYONLY)
    configure_file("${_imgui}/LICENSE.txt" "${CMAKE_BINARY_DIR}/bin/licenses/Dear-ImGui.txt" COPYONLY)
    target_link_libraries(recomp_rdp_gles2 PRIVATE recomp_gles2_cheats)
    target_compile_definitions(recomp_rdp_gles2 PRIVATE RECOMP_GLES2_CHEATS)
endif()
if(RECOMP_GLES2_TEXTURE_PACKS AND NOT EMSCRIPTEN)
    add_library(recomp_gles2_miniz STATIC third_party/miniz/miniz.c)
    target_compile_definitions(recomp_gles2_miniz PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_sources(recomp_rdp_gles2 PRIVATE backends/gles2/texture_pack.c backends/gles2/rice.c
        backends/gles2/editor.c backends/gles2/picker.c backends/gles2/editor_ai.c backends/gles2/editor_ai.h backends/gles2/rice.h backends/gles2/tile.h backends/gles2/texture_pack.h backends/gles2/editor.h)
    target_include_directories(recomp_rdp_gles2 SYSTEM PRIVATE third_party/miniz third_party/stb)
    target_link_libraries(recomp_rdp_gles2 PRIVATE recomp_gles2_miniz)
    set(RECOMP_GLES2_HAS_TEXTURE_PACKS ON)
    target_compile_definitions(recomp_rdp_gles2 PRIVATE RECOMP_GLES2_TEXTURE_PACKS)
    foreach(notice miniz stb)
        configure_file("${PROJECT_SOURCE_DIR}/licenses/${notice}.txt" "${CMAKE_BINARY_DIR}/bin/licenses/${notice}.txt" COPYONLY)
    endforeach()
endif()
if(MSVC)
    target_compile_options(recomp_rdp_gles2 PRIVATE /W4 /fp:strict)
else()
    target_compile_options(recomp_rdp_gles2 PRIVATE -Wall -Wextra -Werror -g1 -fno-omit-frame-pointer -fno-fast-math -ffp-contract=off)
endif()
add_library(recomp_renderer STATIC backends/gles2/renderer.c)
target_link_libraries(recomp_renderer PUBLIC recomp_runtime recomp_rsp recomp_rdp_gles2 SDL2::SDL2)
if(TARGET recomp_gles2_cheats)
    target_compile_definitions(recomp_renderer PRIVATE RECOMP_GLES2_CHEATS)
endif()
if(RECOMP_GLES2_HAS_TEXTURE_PACKS)
    target_compile_definitions(recomp_renderer PRIVATE RECOMP_GLES2_TEXTURE_PACKS)
endif()
if(MSVC)
    target_compile_options(recomp_renderer PRIVATE /W4)
else()
    target_compile_options(recomp_renderer PRIVATE -Wall -Wextra -Werror -g1 -fno-omit-frame-pointer)
endif()
