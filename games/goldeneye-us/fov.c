#include "fov.h"
#include "goldeneye_fov_config.h"
#include "goldeneye_input_config.h"
#include "fov_control.h"
#include <math.h>
#include <string.h>

static bool enabled;
static RecompFov fov;
static double tangent_scale;

void goldeneye_fov_init(void) {
    float native;
    uint32_t bits = GE_FOV_NATIVE_BITS;
    memcpy(&native, &bits, sizeof(native));
    recomp_fov_init(&fov, native, 4.0 / 3.0);
    enabled = false;
}
static void goldeneye_fov_set(double horizontal) {
    recomp_fov_set(&fov, horizontal);
    enabled = recomp_fov_enabled(&fov);
    const double radians = 3.14159265358979323846 / 180.0;
    float native;
    uint32_t bits = GE_FOV_NATIVE_BITS;
    memcpy(&native, &bits, sizeof(native));
    tangent_scale = tan(fov.horizontal * radians / 2) / ((4.0 / 3.0) * tan(native * radians / 2));
}
void goldeneye_fov_adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps)) goldeneye_fov_set(fov.horizontal);
}
static bool eligible(Runtime *rt) {
    uint32_t p = recomp_load32(rt, GE_INPUT_CURRENT_PLAYER);
    if (p < 0x80000000 || p >= 0x80400000 || (p & 3) ||
        (p & 0x1fffffff) + GE_INPUT_WATCH_STATE + 4 > rt->rdram_size ||
        recomp_load32(rt, GE_INPUT_PLAYERS) != p ||
        recomp_load32(rt, GE_INPUT_PLAYERS + 4) || recomp_load32(rt, GE_INPUT_PLAYERS + 8) ||
        recomp_load32(rt, GE_INPUT_PLAYERS + 12) ||
        recomp_load32(rt, GE_FOV_STAGE_ADDRESS) == GE_FOV_TITLE_STAGE ||
        recomp_load32(rt, GE_INPUT_CONTROLS_LOCKED) || recomp_load32(rt, p + GE_INPUT_DEAD) ||
        recomp_load32(rt, p + GE_INPUT_WATCH_STATE)) return false;
    uint32_t camera = recomp_load32(rt, GE_INPUT_CAMERA);
    return camera == 0 || (camera == 4 && recomp_load32(rt, GE_INPUT_TIMER_ACTIVE));
}
void goldeneye_fov_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t caller = (uint32_t)ctx->gpr[31];
    if (enabled && (caller == GE_FOV_VIEWPORT_RETURN || caller == GE_FOV_ZOOM_RETURN ||
                    caller == GE_FOV_RENDER_RETURN) && eligible(rt)) {
        float native;
        memcpy(&native, &ctx->fpr[12], sizeof(native));
        if (isfinite(native) && native > 0 && native < 180) {
            const double radians = 3.14159265358979323846 / 180.0;
            float adjusted = (float)(2 * atan(tan(native * radians / 2) * tangent_scale) / radians);
            memcpy(&ctx->fpr[12], &adjusted, sizeof(adjusted));
        }
    }
    // viSetFovY stores this same value in the VI state, then calls the original
    // player perspective/scale routines used by culling and screen projection.
    // The player's native zoom fields are left intact: each call receives an
    // unadjusted input, so consecutive updates cannot compound the override.
    original(rt, ctx);
}
