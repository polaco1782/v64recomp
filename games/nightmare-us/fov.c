#include "fov.h"
#include "fov_control.h"
#include "nightmare_fov_config.h"
#include "nightmare_input.h"
#include <math.h>
#include <string.h>

static const double radians = 0.017453292519943295;
static const double stock_aspect = 4.0 / 3.0;
static const double stock_vertical = 52.0;
static RecompFov fov;
static uint32_t requested_vertical;

static void adjust(int steps) {
    if (!recomp_fov_adjust(&fov, steps))
        return;
    if (!recomp_fov_enabled(&fov)) {
        requested_vertical = 0;
        return;
    }
    float vertical = (float)(2 * atan(tan(fov.horizontal * radians / 2) / stock_aspect) / radians);
    memcpy(&requested_vertical, &vertical, sizeof(vertical));
}

void nightmare_fov_init(RecompRenderer *renderer) {
    recomp_fov_init(&fov, stock_vertical, stock_aspect);
    requested_vertical = 0;
    recomp_renderer_set_fov_hook(renderer, adjust);
}

void nightmare_fov_perspective(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (requested_vertical && (uint32_t)ctx->gpr[31] == NC_FOV_PERSPECTIVE_RETURN &&
        (uint32_t)ctx->gpr[6] == UINT32_C(0x42500000) && (uint32_t)ctx->gpr[7] == UINT32_C(0x3faaaaab) &&
        nightmare_world_playable(rt)) {
        // Update the original matrix producer, not a renderer's copy.
        ctx->gpr[6] = mips_s32(requested_vertical);
    }
    original(rt, ctx);
}
