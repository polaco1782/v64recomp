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
    backends/gles2/rdp.c
    backends/gles2/render_queue.c
    backends/gles2/context.c
    backends/gles2/controls.c
    backends/gles2/framebuffer.c
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
target_compile_definitions(recomp_rdp_gles2 PUBLIC RECOMP_WITH_GLES2 PRIVATE SDL_MAIN_HANDLED)

# Native PNG/ZIP support reuses the pinned v5 texture identity and standalone
# codecs; it does not build or link RT64. Keep plain GLES2 usable on WebAssembly
# and in trees configured without the RT64 submodule.
option(RECOMP_GLES2_TEXTURE_PACKS "Load and edit native GLES texture packs" ON)
set(_gles2_texture_source "${PROJECT_SOURCE_DIR}/third_party/rt64/src")
set(_imgui "${_gles2_texture_source}/contrib/imgui")
option(RECOMP_GLES2_CHEATS "Build the GameShark overlay from the pinned Dear ImGui sources" ON)
if(RECOMP_GLES2_CHEATS AND EXISTS "${_imgui}/imgui.cpp")
    enable_language(CXX)
    add_library(recomp_gles2_cheats STATIC
        backends/cheat_database.cpp backends/cheat_panel.cpp backends/gles2/cheats.cpp
        "${_imgui}/imgui.cpp" "${_imgui}/imgui_draw.cpp" "${_imgui}/imgui_tables.cpp" "${_imgui}/imgui_widgets.cpp"
        "${_imgui}/backends/imgui_impl_sdl2.cpp" "${_imgui}/backends/imgui_impl_opengl3.cpp")
    target_compile_features(recomp_gles2_cheats PRIVATE cxx_std_17)
    target_compile_definitions(recomp_gles2_cheats PRIVATE IMGUI_IMPL_OPENGL_ES3 SDL_MAIN_HANDLED)
    target_include_directories(recomp_gles2_cheats SYSTEM PRIVATE "${_imgui}")
    target_link_libraries(recomp_gles2_cheats PRIVATE recomp_runtime SDL2::SDL2)
    if(EMSCRIPTEN)
        target_compile_options(recomp_gles2_cheats PRIVATE -fexceptions)
        target_link_options(recomp_gles2_cheats INTERFACE -fexceptions)
    else()
        find_package(CURL 7.85 REQUIRED)
        find_path(RECOMP_GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
        find_library(RECOMP_GLES_LIBRARY NAMES GLESv2 libGLESv2 REQUIRED)
        target_include_directories(recomp_gles2_cheats SYSTEM PRIVATE "${RECOMP_GLES3_INCLUDE_DIR}")
        target_link_libraries(recomp_gles2_cheats PRIVATE CURL::libcurl "${RECOMP_GLES_LIBRARY}")
    endif()
    configure_file("${_imgui}/LICENSE.txt" "${CMAKE_BINARY_DIR}/bin/licenses/Dear-ImGui.txt" COPYONLY)
    target_link_libraries(recomp_rdp_gles2 PRIVATE recomp_gles2_cheats)
    target_compile_definitions(recomp_rdp_gles2 PRIVATE RECOMP_GLES2_CHEATS)
elseif(RECOMP_GLES2_CHEATS)
    message(STATUS "GLES cheats disabled: initialize the pinned Dear ImGui submodule to enable the overlay")
endif()
if(RECOMP_GLES2_TEXTURE_PACKS AND NOT EMSCRIPTEN)
    if(EXISTS "${_gles2_texture_source}/common/rt64_tmem_hasher.h"
            AND EXISTS "${_gles2_texture_source}/common/rt64_rice_hasher.cpp"
            AND EXISTS "${_gles2_texture_source}/contrib/hlslpp/include/hlsl++.h"
            AND EXISTS "${_gles2_texture_source}/contrib/miniz/miniz.c"
            AND EXISTS "${_gles2_texture_source}/contrib/xxHash/xxhash.h"
            AND EXISTS "${_gles2_texture_source}/contrib/json/json.hpp"
            AND EXISTS "${_gles2_texture_source}/contrib/stb/stb_image.h"
            AND EXISTS "${_gles2_texture_source}/contrib/stb/stb_easy_font.h")
        enable_language(CXX)
        add_library(recomp_gles2_miniz STATIC "${_gles2_texture_source}/contrib/miniz/miniz.c")
        target_compile_definitions(recomp_gles2_miniz PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
        add_library(recomp_gles2_rice STATIC
            "${_gles2_texture_source}/common/rt64_rice_hasher.cpp"
            "${_gles2_texture_source}/common/rt64_replacement_database.cpp")
        target_include_directories(recomp_gles2_rice SYSTEM PRIVATE "${_gles2_texture_source}"
            "${_gles2_texture_source}/contrib" "${_gles2_texture_source}/contrib/hlslpp/include")
        target_sources(recomp_rdp_gles2 PRIVATE
            backends/gles2/texture_pack.cpp backends/gles2/texture_pack.h backends/gles2/tile.h
            backends/gles2/editor.cpp backends/gles2/editor.h)
        set_target_properties(recomp_rdp_gles2 recomp_gles2_rice PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(recomp_rdp_gles2 SYSTEM PRIVATE
            "${_gles2_texture_source}" "${_gles2_texture_source}/contrib")
        target_link_libraries(recomp_rdp_gles2 PRIVATE recomp_gles2_miniz recomp_gles2_rice)
        set(RECOMP_GLES2_HAS_TEXTURE_PACKS ON)
        target_compile_definitions(recomp_rdp_gles2 PRIVATE RECOMP_GLES2_TEXTURE_PACKS)
        configure_file("${_gles2_texture_source}/contrib/hlslpp/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/hlslpp.txt" COPYONLY)
        foreach(notice miniz nlohmann-json stb xxHash)
            configure_file("${PROJECT_SOURCE_DIR}/licenses/${notice}.txt"
                "${CMAKE_BINARY_DIR}/bin/licenses/${notice}.txt" COPYONLY)
        endforeach()
    else()
        message(STATUS "GLES texture packs disabled: pinned PNG/ZIP and v5 hash sources are not available")
    endif()
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
