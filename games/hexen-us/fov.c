#include "fov.h"
#include "game.h"
#include "hexen_fov_config.h"
#include "fov_control.h"
#include <math.h>
#include <string.h>

static bool enabled;
static RecompFov fov;
static uint32_t cull_bits;
static double projection_scale;
static Runtime *projection_rt, *visibility_rt;
static uint32_t native_projection[16];
static uint32_t native_table[4096], native_clip;

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
/* Hexen caches this fixed matrix between camera/level setup calls. Updating
 * guPerspective's argument alone cannot change the view while playing. Always
 * scale the saved native matrix so repeated hotkeys never compound rounding. */
static void update_projection(void) {
    Runtime *rt = projection_rt;
    if (!rt)
        return;
    uint32_t matrix = HEXEN_FOV_PROJECTION_MATRIX;
    for (unsigned i = 0; i < 16; i++)
        recomp_store32(rt, matrix + i * 4, native_projection[i]);
    if (!enabled)
        return;
    const unsigned diagonal[] = {0, 5};
    for (unsigned i = 0; i < 2; i++) {
        unsigned offset = diagonal[i] * 2;
        int32_t fixed =
            (int32_t)((int16_t)recomp_load16(rt, matrix + offset) * 65536.0 + recomp_load16(rt, matrix + 32 + offset));
        int32_t adjusted = (int32_t)(fixed * projection_scale);
        recomp_store16(rt, matrix + offset, (uint16_t)((uint32_t)adjusted >> 16));
        recomp_store16(rt, matrix + 32 + offset, (uint16_t)adjusted);
    }
}
/* The original initializer allocates/frees a tangent buffer. Save its finished
 * tables once; hotkeys update them without rerunning its allocator or patching
 * guest code. Widening past atan(2) requires removing its fixed +/-2 cutoff. */
static void update_visibility(void) {
    Runtime *rt = visibility_rt;
    if (!rt)
        return;
    if (!enabled || cull_bits == HEXEN_FOV_CULL_BITS) {
        for (unsigned i = 0; i < 4096; i++)
            recomp_store32(rt, HEXEN_FOV_VISIBILITY_TABLE + i * 4, native_table[i]);
        recomp_store32(rt, HEXEN_FOV_CLIP_ANGLE, native_clip);
        return;
    }
    const double radians = 3.14159265358979323846 / 180;
    float center = from_bits(HEXEN_FOV_SCREEN_CENTER_BITS);
    float maximum = from_bits(HEXEN_FOV_SCREEN_MAX_BITS);
    float half = (float)(from_bits(cull_bits) * 360.0 / HEXEN_FOV_FINE_ANGLES * radians);
    float focal = center / (sinf(half) / cosf(half));
    unsigned first_negative = 4096;
    for (unsigned i = 0; i < 4096; i++) {
        float angle = (float)(((double)i - 2048 + 0.5) * (2 * 3.14159265358979323846) / HEXEN_FOV_FINE_ANGLES);
        float tangent = sinf(angle) / cosf(angle);
        float projected = center - tangent * focal;
        int32_t x = (int32_t)fmaxf(-1, fminf(maximum, ceilf(projected)));
        recomp_store32(rt, HEXEN_FOV_VISIBILITY_TABLE + i * 4, (uint32_t)x);
        if (x < 0 && first_negative == 4096)
            first_negative = i;
    }
    recomp_store32(rt, HEXEN_FOV_CLIP_ANGLE, (first_negative << 19) - UINT32_C(0x40000000));
}
void hexen_fov_init(void) {
    recomp_fov_init(&fov, from_bits(HEXEN_FOV_VERTICAL_BITS), from_bits(HEXEN_FOV_ASPECT_BITS));
    enabled = false;
    projection_rt = visibility_rt = NULL;
}
static void hexen_fov_set(double horizontal) {
    recomp_fov_set(&fov, horizontal);
    enabled = recomp_fov_enabled(&fov);
    horizontal = fov.horizontal;
    const double radians = 3.14159265358979323846 / 180.0;
    projection_scale = tan(fov.original * radians / 2) / tan(horizontal * radians / 2);
    // Keep the stock conservative span until a wider view needs more coverage.
    double fine = (horizontal / 2 + 1) * HEXEN_FOV_FINE_ANGLES / 360.0;
    cull_bits = to_bits((float)fmax(from_bits(HEXEN_FOV_CULL_BITS), fine));
    update_projection();
    update_visibility();
}
void hexen_fov_adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps))
        hexen_fov_set(fov.horizontal);
}
void hexen_fov_projection_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // The support-code intro has its own 45-degree perspective call. Only the
    // verified world matrix uses this override; orthographic HUD/map calls do not.
    bool world =
        (uint32_t)ctx->gpr[31] == HEXEN_FOV_PROJECTION_RETURN && (uint32_t)ctx->gpr[4] == HEXEN_FOV_PROJECTION_MATRIX;
    if (world) {
        recomp_game_require((uint32_t)ctx->gpr[6] == HEXEN_FOV_VERTICAL_BITS &&
                                (uint32_t)ctx->gpr[7] == HEXEN_FOV_ASPECT_BITS,
                            "Hexen FOV projection ABI mismatch", HEXEN_FOV_PROJECTION_RETURN);
    }
    original(rt, ctx);
    if (world) {
        for (unsigned i = 0; i < 16; i++)
            native_projection[i] = recomp_load32(rt, HEXEN_FOV_PROJECTION_MATRIX + i * 4);
        projection_rt = rt;
        update_projection();
    }
}
void hexen_fov_culling_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    recomp_game_require(recomp_load32(rt, HEXEN_FOV_CULL_ANGLE) == HEXEN_FOV_CULL_BITS,
                        "Hexen FOV visibility constant mismatch", HEXEN_FOV_CULL_ANGLE);
    original(rt, ctx);
    for (unsigned i = 0; i < 4096; i++)
        native_table[i] = recomp_load32(rt, HEXEN_FOV_VISIBILITY_TABLE + i * 4);
    native_clip = recomp_load32(rt, HEXEN_FOV_CLIP_ANGLE);
    visibility_rt = rt;
    update_visibility();
}
