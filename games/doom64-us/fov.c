#include "fov.h"
#include "doom_fov_config.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>

static const double radians = 3.14159265358979323846 / 180.0;
static RecompFov fov;
static int32_t selected_scale = 65536, world_scale = 65536;

static void adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps))
        selected_scale =
            recomp_fov_enabled(&fov) ? (int32_t)lround(65536.0 / tan(fov.horizontal * radians / 2)) : 65536;
}

void doom64_fov_init(RecompRenderer *renderer) {
    // R_Init's symmetric frustum is eight units deep and sixteen units wide.
    recomp_fov_init(&fov, 2 * atan(6.0 / 8) / radians, 4.0 / 3);
    recomp_renderer_set_fov_hook(renderer, adjust);
}

void doom64_world_view(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (selected_scale != 65536 && (uint32_t)ctx->gpr[31] == DOOM_FOV_WORLD_RETURN &&
        !recomp_load32(rt, DOOM_FOV_DEMO_PLAYBACK) &&
        recomp_load32(rt, DOOM_FOV_CAMERA) == recomp_load32(rt, DOOM_FOV_PLAYER)) {
        world_scale = selected_scale;
    }
    original(rt, ctx);
    world_scale = 65536;
}

int32_t doom64_fov_lateral(int32_t x) {
    // BSP extents, cached wall endpoints and actor side tests keep their native
    // depth. This maps a wider view onto Doom's same 320 occlusion columns.
    return world_scale == 65536 ? x : (int32_t)((int64_t)x * world_scale / 65536);
}

void doom64_world_geometry(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (world_scale != 65536 && (uint32_t)ctx->gpr[31] == DOOM_FOV_GEOMETRY_RETURN) {
        // Each bank holds four matrices. Doom just advanced past the three
        // model matrices and has drawn its sky using the stock projection.
        uint32_t matrix = recomp_load32(rt, DOOM_FOV_MATRIX_CURSOR);
        recomp_game_require(matrix == DOOM_FOV_MATRIX_POOL + 3 * 64 || matrix == DOOM_FOV_MATRIX_POOL + 7 * 64,
                            "unexpected Doom 64 world matrix cursor", matrix);
        uint32_t gfx = recomp_load32(rt, DOOM_FOV_GFX_CURSOR);
        uint32_t base = recomp_load32(rt, DOOM_FOV_GFX_BASE);
        recomp_game_require(gfx >= base && gfx - base < 5120 * 8 && !(gfx & 7),
                            "unexpected Doom 64 world display list cursor", gfx);
        for (unsigned i = 0; i < 64; i += 4)
            recomp_store32(rt, matrix + i, recomp_load32(rt, DOOM_FOV_PROJECTION + i));
        // N64 Mtx stores the integer halves first, followed by the fractions.
        for (unsigned diagonal = 0; diagonal <= 5; diagonal += 5) {
            uint32_t word = (uint32_t)recomp_load16(rt, matrix + 2 * diagonal) << 16 |
                            recomp_load16(rt, matrix + 32 + 2 * diagonal);
            uint32_t scaled = ((uint64_t)word * (uint32_t)world_scale + 32768) >> 16;
            recomp_store16(rt, matrix + 2 * diagonal, scaled >> 16);
            recomp_store16(rt, matrix + 32 + 2 * diagonal, (uint16_t)scaled);
        }
        recomp_store32(rt, gfx, 0x01030040); // gSPMatrix(PROJECTION | LOAD | NOPUSH)
        recomp_store32(rt, gfx + 4, matrix & 0x1fffffff);
        recomp_store32(rt, DOOM_FOV_GFX_CURSOR, gfx + 8);
        recomp_store32(rt, DOOM_FOV_MATRIX_CURSOR, matrix + 64);
    }
    original(rt, ctx);
}
