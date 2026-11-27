#include "fov.h"
#include "doom_fov_config.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>

static const double radians = 3.14159265358979323846 / 180.0;
static RecompFov fov;
static int32_t selected_scale = 65536, world_scale = 65536, visibility_scale = 65536;
static double mouse_pitch, pitch_limit = 35;
static int32_t world_pitch;
static double sky_scale = 1;

void doom64_adjust_pitch(double degrees) {
    mouse_pitch = fmax(-pitch_limit, fmin(pitch_limit, mouse_pitch + degrees));
}

void doom64_reset_pitch(void) {
    mouse_pitch = 0;
}

static void adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps)) {
        selected_scale =
            recomp_fov_enabled(&fov) ? (int32_t)lround(65536.0 / tan(fov.horizontal * radians / 2)) : 65536;
        // Keep even the steepest corner in front of the horizontal BSP near plane,
        // allowing headroom for the native weapon kick.
        pitch_limit = fmin(35.0, 80.0 - atan(0.75 * 65536 / selected_scale) / radians);
        doom64_adjust_pitch(0);
    }
}

void doom64_fov_init(RecompRenderer *renderer) {
    // R_Init's symmetric frustum is eight units deep and sixteen units wide.
    recomp_fov_init(&fov, 2 * atan(6.0 / 8) / radians, 4.0 / 3);
    recomp_renderer_set_fov_hook(renderer, adjust);
}

void doom64_world_view(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if ((mouse_pitch || selected_scale != 65536) && (uint32_t)ctx->gpr[31] == DOOM_FOV_WORLD_RETURN &&
        !recomp_load32(rt, DOOM_FOV_DEMO_PLAYBACK) &&
        recomp_load32(rt, DOOM_FOV_CAMERA) == recomp_load32(rt, DOOM_FOV_PLAYER)) {
        world_scale = visibility_scale = selected_scale;
        world_pitch = (int32_t)lround(mouse_pitch * (8192.0 / 360.0));
    }
    original(rt, ctx);
    world_scale = visibility_scale = 65536;
    world_pitch = 0;
    sky_scale = 1;
}

int32_t doom64_view_pitch(int32_t recoil) {
    if (!world_pitch)
        return recoil;
    int32_t pitch = (uint32_t)(world_pitch + recoil) & 8191;
    // The nearest horizontal depth of a tilted frustum corner, relative to
    // eye-space depth, is cos(p) - tan(vertical FOV / 2) * abs(sin(p)).
    // Apply this to Doom's column tests only. The GPU uses the real rotation.
    double angle = pitch * (360.0 / 8192) * radians;
    visibility_scale = (int32_t)fmax(1, floor(world_scale * cos(angle) - (0.75 * 65536) * fabs(sin(angle))));
    // The backdrop is drawn before the world rotation. Its original horizon
    // is row 120, with a vertical focal length of 160 at the stock FOV.
    sky_scale = 1 + (160.0 / 120) * (world_scale / 65536.0) * tan(angle);
    return pitch;
}

void doom64_sky_quad(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    original(rt, ctx);
    if (sky_scale == 1)
        return;
    // Cloud and fire each emit four frame-owned vertices under the stock
    // frustum. Scale their projected rows about the top of the viewport.
    uint32_t end = recomp_load32(rt, DOOM_FOV_VERTEX_CURSOR);
    for (uint32_t v = end - 4 * 16; v < end; v += 16) {
        int16_t y = (int16_t)recomp_load16(rt, v + 2);
        int16_t z = (int16_t)recomp_load16(rt, v + 4);
        double top = -0.75 * z;
        recomp_store16(rt, v + 2, (int16_t)lround(top + (y - top) * sky_scale));
    }
}

void doom64_sky_picture(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t start = recomp_load32(rt, DOOM_FOV_GFX_CURSOR);
    original(rt, ctx);
    if (sky_scale == 1)
        return;
    // Space and mountains are texture rectangles assembled from horizontal
    // strips. Scale their shared edges and compensate the texture step.
    uint32_t end = recomp_load32(rt, DOOM_FOV_GFX_CURSOR);
    for (uint32_t gfx = start; gfx + 24 <= end; gfx += 8) {
        uint32_t rect = recomp_load32(rt, gfx);
        if ((rect >> 24) != 0xe4)
            continue;
        uint32_t origin = recomp_load32(rt, gfx + 4);
        uint32_t bottom = (uint32_t)lround((rect & 0xfff) * sky_scale);
        uint32_t top = (uint32_t)lround((origin & 0xfff) * sky_scale);
        recomp_store32(rt, gfx, (rect & ~0xfffu) | (bottom > 960 ? 960 : bottom));
        recomp_store32(rt, gfx + 4, (origin & ~0xfffu) | (top > 960 ? 960 : top));
        recomp_store16(rt, gfx + 22, (uint16_t)lround(1024 / sky_scale));
        gfx += 16;
    }
}

int32_t doom64_fov_lateral(int32_t x) {
    // BSP extents, cached wall endpoints and actor side tests keep their native
    // depth. This maps a wider view onto Doom's same 320 occlusion columns.
    return visibility_scale == 65536 ? x : (int32_t)((int64_t)x * visibility_scale / 65536);
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
