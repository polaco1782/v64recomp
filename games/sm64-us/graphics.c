#include "graphics.h"
#include "game_config.h"

void sm64_graphics_bind(RecompRenderer *renderer) {
#ifdef RECOMP_WITH_GLES2
    recomp_renderer_graphics_identity(renderer, SM64_GFX_CODE_SHA256, SM64_GFX_CODE_SIZE, SM64_GFX_DATA_SHA256,
                                      SM64_GFX_DATA_SIZE);
#else
    (void)renderer;
#endif
}
