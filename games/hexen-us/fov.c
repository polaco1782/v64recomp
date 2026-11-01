#include "fov.h"
#include "game.h"
#include "hexen_fov_config.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled;
static uint32_t vertical_bits, cull_bits;

static float from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint32_t to_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
bool hexen_fov_configure(const char *setting) {
    enabled = false;
    if (!setting || !*setting) return true;
    char *end;
    errno = 0;
    double horizontal = strtod(setting, &end);
    if (end == setting || *end || errno == ERANGE || !isfinite(horizontal) ||
        (horizontal != 0 && (horizontal < 60 || horizontal > 120))) return false;
    if (horizontal == 0) return true;
    const double radians = 3.14159265358979323846 / 180.0;
    double aspect = from_bits(HEXEN_FOV_ASPECT_BITS);
    vertical_bits = to_bits((float)(2 * atan(tan(horizontal * radians / 2) / aspect) / radians));
    // Keep the stock conservative visibility span for narrower views. Wider
    // views need at least one degree of overscan on either side. At the maximum
    // supported FOV this stays below the native initializer's tan(angle)=2 cut.
    double fine = (horizontal / 2 + 1) * HEXEN_FOV_FINE_ANGLES / 360.0;
    cull_bits = to_bits((float)fmax(from_bits(HEXEN_FOV_CULL_BITS), fine));
    enabled = true;
    return true;
}
void hexen_fov_projection_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // The support-code intro has its own 45-degree perspective call. Only the
    // verified world matrix uses this override; orthographic HUD/map calls do not.
    if (enabled && (uint32_t)ctx->gpr[31] == HEXEN_FOV_PROJECTION_RETURN &&
        (uint32_t)ctx->gpr[4] == HEXEN_FOV_PROJECTION_MATRIX) {
        recomp_game_require((uint32_t)ctx->gpr[6] == HEXEN_FOV_VERTICAL_BITS &&
                            (uint32_t)ctx->gpr[7] == HEXEN_FOV_ASPECT_BITS,
                            "Hexen FOV projection ABI mismatch", HEXEN_FOV_PROJECTION_RETURN);
        // guPerspective's o32 ABI carries fovy as float bits in a2, not f12.
        ctx->gpr[6] = mips_s32(vertical_bits);
    }
    original(rt, ctx);
}
void hexen_fov_culling_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (!enabled || cull_bits == HEXEN_FOV_CULL_BITS) {
        original(rt, ctx);
        return;
    }
    uint32_t saved = recomp_load32(rt, HEXEN_FOV_CULL_ANGLE);
    recomp_game_require(saved == HEXEN_FOV_CULL_BITS, "Hexen FOV visibility constant mismatch",
                        HEXEN_FOV_CULL_ANGLE);
    // This scalar has one checked reader, inside the original table initializer.
    // Let that body rebuild both angle-to-screen and clip-angle data, then restore
    // the source constant. Allocation, rounding and table bounds stay guest code.
    recomp_store32(rt, HEXEN_FOV_CULL_ANGLE, cull_bits);
    original(rt, ctx);
    recomp_store32(rt, HEXEN_FOV_CULL_ANGLE, saved);
}
