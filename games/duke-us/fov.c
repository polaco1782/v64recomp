#include "fov.h"
#include "duke_fov_config.h"
#include "fov_control.h"
#include <math.h>
#include <string.h>

static bool enabled;
static RecompFov fov;
static uint32_t vertical_bits;
static float scan_padding;
static float sprite_scale;
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
void duke_fov_init(void) {
    recomp_fov_init(&fov, from_bits(DUKE_FOV_VERTICAL_BITS), from_bits(DUKE_FOV_ASPECT_BITS));
    enabled = false;
}
static void duke_fov_set(double horizontal) {
    recomp_fov_set(&fov, horizontal);
    enabled = recomp_fov_enabled(&fov);
    horizontal = fov.horizontal;
    const double radians = 3.14159265358979323846 / 180.0;
    vertical_bits = to_bits((float)(2 * atan(tan(horizontal * radians / 2) /
                                           from_bits(DUKE_FOV_ASPECT_BITS)) / radians));
    sprite_scale = (float)(tan(from_bits(DUKE_FOV_VERTICAL_BITS) * radians / 2) /
                           tan(from_bits(vertical_bits) * radians / 2));
    // Preserve the native pitch-dependent overscan and never narrow the scan.
    // Add one degree beyond the requested half-angle to its stock 35 degrees.
    scan_padding = (float)fmax(0, horizontal / 2 + 1 - from_bits(DUKE_FOV_SCAN_HALF_BITS));
}
void duke_fov_adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps)) duke_fov_set(fov.horizontal);
}
void duke_fov_projection_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (!enabled) {
        original(rt, ctx);
        return;
    }
    uint32_t caller = (uint32_t)ctx->gpr[31];
    bool world = caller == DUKE_FOV_CPU_RETURN;
    if (caller == DUKE_FOV_INNER_RETURN) {
        uint32_t sp = (uint32_t)ctx->gpr[29];
        if (sp >= 0x80000000 && sp < 0x80800000 && !(sp & 3) &&
            (sp & 0x1fffffff) + DUKE_FOV_SAVED_RA_OFFSET + 4 <= rt->rdram_size)
            world = recomp_load32(rt, sp + DUKE_FOV_SAVED_RA_OFFSET) == DUKE_FOV_GPU_RETURN;
    }
    if (world && recomp_load16(rt, DUKE_FOV_PLAYERS) == 1 &&
        (uint32_t)ctx->gpr[6] == DUKE_FOV_VERTICAL_BITS &&
        (uint32_t)ctx->gpr[7] == DUKE_FOV_ASPECT_BITS)
        ctx->gpr[6] = mips_s32(vertical_bits);
    // The original F routine supplies both the CPU float matrix (later combined
    // with the view) and the GPU fixed matrix through guPerspective's F2L call.
    original(rt, ctx);
}
void duke_fov_sprite_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t caller = (uint32_t)ctx->gpr[31];
    if (enabled && recomp_load16(rt, DUKE_FOV_PLAYERS) == 1 &&
        (caller == DUKE_FOV_SPRITE_RETURN || caller == DUKE_FOV_EFFECT_RETURN)) {
        // World billboard anchors already use the adjusted float matrix. Their
        // depth-derived sizes bypass it; apply the same focal-length ratio here.
        ctx->gpr[6] = mips_s32(to_bits(from_bits((uint32_t)ctx->gpr[6]) * sprite_scale));
        ctx->gpr[7] = mips_s32(to_bits(from_bits((uint32_t)ctx->gpr[7]) * sprite_scale));
    }
    original(rt, ctx);
}
void duke_fov_visibility_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (enabled && scan_padding > 0 && (uint32_t)ctx->gpr[31] == DUKE_FOV_SCAN_RETURN &&
        recomp_load16(rt, DUKE_FOV_PLAYERS) == 1) {
        // Only widen the first scan. Recursive portal windows retain their
        // original clipping, visited lists, sorting and side effects.
        ctx->fpr[12] = to_bits(from_bits(ctx->fpr[12]) - scan_padding);
        ctx->fpr[14] = to_bits(from_bits(ctx->fpr[14]) + scan_padding);
    }
    original(rt, ctx);
}
