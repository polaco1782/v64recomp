# Loaded only by the Emscripten toolchain. Desktop defaults stay native.
set(RECOMP_RENDERER gles2 CACHE STRING "Graphics backend")
set(RECOMP_BUILD_RT64 OFF CACHE BOOL "Build RT64")
if(NOT RECOMP_RENDERER STREQUAL "gles2" OR RECOMP_BUILD_RT64)
    message(FATAL_ERROR "Browser builds require GLES2 and RECOMP_BUILD_RT64=OFF")
endif()
add_compile_options(-pthread -Wno-unsupported-floating-point-opt)
add_link_options(-pthread)

add_library(SDL2::SDL2 INTERFACE IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
    INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2"
    INTERFACE_LINK_OPTIONS "-sUSE_SDL=2")
