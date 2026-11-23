#include "fov.h"
#include "fov_control.h"
#include "quake2_fov_config.h"
#include "quake2_input.h"
#include <math.h>
#include <string.h>

static const double radians = 0.017453292519943295;
static const double stock_aspect = 4.0 / 3.0;
static RecompFov fov;
static double view_scale = 1.0, world_scale = 1.0;

static void adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps))
        view_scale = recomp_fov_enabled(&fov) ? 1 / tan(fov.horizontal * radians / 2) : 1;
}

void quake2_fov_init(RecompRenderer *renderer) {
    // The single-player frustum is +/-4 horizontally, +/-3 vertically, at z=4.
    recomp_fov_init(&fov, 2 * atan(3.0 / 4.0) / radians, stock_aspect);
    view_scale = world_scale = 1;
    recomp_renderer_set_fov_hook(renderer, adjust);
}

static uint32_t read_matrix_word(Runtime *rt, uint32_t address) {
    return (uint32_t)recomp_load16(rt, address) << 16 | recomp_load16(rt, address + 32);
}

static void write_matrix_word(Runtime *rt, uint32_t address, uint32_t word) {
    recomp_store16(rt, address, word >> 16);
    recomp_store16(rt, address + 32, (uint16_t)word);
}

void quake2_fov_world(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (view_scale == 1 || (uint32_t)ctx->gpr[31] != Q2_FOV_WORLD_RETURN || !quake2_world_playable(rt)) {
        original(rt, ctx);
        return;
    }
    uint32_t x = read_matrix_word(rt, Q2_FOV_PROJECTION);
    uint32_t y = read_matrix_word(rt, Q2_FOV_PROJECTION + 10);
    if (x != UINT32_C(0x10000) || y != UINT32_C(0x15555)) {
        original(rt, ctx);
        return;
    }
    // Keep the matrix wide through the final list flush, not just the nested
    // world draw. Other callers of the guest matrix still see exact stock.
    world_scale = view_scale;
    write_matrix_word(rt, Q2_FOV_PROJECTION, (uint32_t)lround(x * world_scale));
    write_matrix_word(rt, Q2_FOV_PROJECTION + 10, (uint32_t)lround(y * world_scale));
    original(rt, ctx);
    write_matrix_word(rt, Q2_FOV_PROJECTION, x);
    write_matrix_word(rt, Q2_FOV_PROJECTION + 10, y);
    world_scale = 1;
}

void quake2_fov_frustum(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (world_scale == 1 || (uint32_t)ctx->gpr[31] != Q2_FOV_FRUSTUM_RETURN) {
        original(rt, ctx);
        return;
    }
    const uint32_t axes[2] = {Q2_FOV_RIGHT_BASIS, Q2_FOV_UP_BASIS};
    uint32_t saved[2][3];
    // The guest constructs four planes as forward +/- right or up and
    // computes plane distances/sign bits. Preserve the camera basis outside
    // this helper and leave BSP visibility and view angles unchanged.
    for (unsigned axis = 0; axis < 2; axis++) {
        for (unsigned component = 0; component < 3; component++) {
            uint32_t address = axes[axis] + 4 * component;
            uint32_t bits = recomp_load32(rt, address);
            saved[axis][component] = bits;
            float value;
            memcpy(&value, &bits, sizeof(value));
            value = (float)(value * world_scale);
            memcpy(&bits, &value, sizeof(bits));
            recomp_store32(rt, address, bits);
        }
    }
    original(rt, ctx);
    for (unsigned axis = 0; axis < 2; axis++)
        for (unsigned component = 0; component < 3; component++)
            recomp_store32(rt, axes[axis] + 4 * component, saved[axis][component]);
}
