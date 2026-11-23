if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED)
endif()
target_sources(recomp_rsp PRIVATE runtime/rsp_worker.c include/rsp_worker.h)
add_library(recomp_rdp STATIC backends/rdp_triangle.c include/rdp_triangle.h)
target_link_libraries(recomp_rdp PUBLIC recomp_runtime)
if(MSVC)
    target_compile_options(recomp_rdp PRIVATE /W4 /fp:strict)
else()
    target_compile_options(recomp_rdp PRIVATE -Wall -Wextra -Werror -g1 -fno-omit-frame-pointer -fno-fast-math -ffp-contract=off)
endif()
add_library(recomp_rdp_gles2 STATIC
    backends/gles2/rdp.c
    backends/gles2/context.c
    backends/gles2/controls.c
    backends/gles2/framebuffer.c
    backends/gles2/textures.c
    backends/gles2/combiner.c
    backends/gles2/wireframe.c
    backends/gles2/internal.h
    backends/input.c
    include/rdp_gles2.h)
target_link_libraries(recomp_rdp_gles2 PUBLIC recomp_rdp SDL2::SDL2)
target_compile_definitions(recomp_rdp_gles2 PUBLIC RECOMP_WITH_GLES2 PRIVATE SDL_MAIN_HANDLED)

# Native PNG/ZIP support reuses the pinned v5 texture identity and standalone
# codecs; it does not build or link RT64. Keep plain GLES2 usable on WebAssembly
# and in trees configured without the RT64 submodule.
option(RECOMP_GLES2_TEXTURE_PACKS "Load and edit native GLES texture packs" ON)
set(_gles2_texture_source "${PROJECT_SOURCE_DIR}/third_party/rt64/src")
if(RECOMP_GLES2_TEXTURE_PACKS AND NOT EMSCRIPTEN)
    if(EXISTS "${_gles2_texture_source}/common/rt64_tmem_hasher.h"
            AND EXISTS "${_gles2_texture_source}/contrib/miniz/miniz.c"
            AND EXISTS "${_gles2_texture_source}/contrib/xxHash/xxhash.h"
            AND EXISTS "${_gles2_texture_source}/contrib/json/json.hpp"
            AND EXISTS "${_gles2_texture_source}/contrib/stb/stb_image.h"
            AND EXISTS "${_gles2_texture_source}/contrib/stb/stb_easy_font.h")
        enable_language(CXX)
        add_library(recomp_gles2_miniz STATIC "${_gles2_texture_source}/contrib/miniz/miniz.c")
        target_compile_definitions(recomp_gles2_miniz PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
        target_sources(recomp_rdp_gles2 PRIVATE
            backends/gles2/texture_pack.cpp backends/gles2/texture_pack.h
            backends/gles2/editor.cpp backends/gles2/editor.h)
        set_target_properties(recomp_rdp_gles2 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(recomp_rdp_gles2 SYSTEM PRIVATE
            "${_gles2_texture_source}" "${_gles2_texture_source}/contrib")
        target_link_libraries(recomp_rdp_gles2 PRIVATE recomp_gles2_miniz)
        set(RECOMP_GLES2_HAS_TEXTURE_PACKS ON)
        target_compile_definitions(recomp_rdp_gles2 PRIVATE RECOMP_GLES2_TEXTURE_PACKS)
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
if(RECOMP_GLES2_HAS_TEXTURE_PACKS)
    target_compile_definitions(recomp_renderer PRIVATE RECOMP_GLES2_TEXTURE_PACKS)
endif()
if(MSVC)
    target_compile_options(recomp_renderer PRIVATE /W4)
else()
    target_compile_options(recomp_renderer PRIVATE -Wall -Wextra -Werror -g1 -fno-omit-frame-pointer)
endif()
