# Loaded only by the Emscripten toolchain. Desktop defaults stay native.
add_compile_options(-pthread -Wno-unsupported-floating-point-opt)
add_link_options(-pthread)

add_library(SDL2::SDL2 INTERFACE IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
    INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2"
    INTERFACE_LINK_OPTIONS "-sUSE_SDL=2")
