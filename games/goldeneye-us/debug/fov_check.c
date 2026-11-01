/* Synthetic wrapper/zoom checks; ROM verification covers the original setter.
 * This does not execute the game's paged camera code or establish gameplay. */
#include "fov.h"
#include "game.h"
#include "goldeneye_fov_config.h"
#include "goldeneye_input_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void goldeneye_fov_hook(Runtime *, MipsContext *, RecompFunction);
static const uint32_t player = 0x80010000;
static unsigned calls;
static float observed;
static void check(bool condition, const char *message) { recomp_game_require(condition, message, 0); }
static uint32_t bits(float value) {
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}
static void observe(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    calls++;
    memcpy(&observed, &ctx->fpr[12], sizeof(observed));
    ctx->gpr[2] = 123; // Observable original return value must survive the wrapper.
}
static MipsContext arguments(float native, uint32_t caller) {
    MipsContext ctx = {0};
    ctx.gpr[31] = caller;
    ctx.fpr[12] = bits(native);
    ctx.fpr[14] = bits(1.3333334f);
    ctx.gpr[4] = 0x12345678;
    return ctx;
}
static void call(Runtime *rt, MipsContext *ctx, bool unchanged) {
    MipsContext expected = *ctx;
    expected.gpr[2] = 123;
    calls = 0;
    goldeneye_fov_hook(rt, ctx, observe);
    check(calls == 1 && ctx->gpr[2] == 123, "GoldenEye FOV: original call/return");
    if (!unchanged) expected.fpr[12] = ctx->fpr[12];
    check(!memcmp(&expected, ctx, sizeof(expected)), "GoldenEye FOV: unexpected argument/context change");
}
static void gameplay(Runtime *rt) {
    recomp_store32(rt, GE_INPUT_CURRENT_PLAYER, player);
    for (unsigned i = 0; i < 4; i++) recomp_store32(rt, GE_INPUT_PLAYERS + i * 4, i ? 0 : player);
    recomp_store32(rt, GE_FOV_STAGE_ADDRESS, 0x21);
    recomp_store32(rt, GE_INPUT_CAMERA, 4);
    recomp_store32(rt, GE_INPUT_TIMER_ACTIVE, 1);
    recomp_store32(rt, GE_INPUT_CONTROLS_LOCKED, 0);
    recomp_store32(rt, player + GE_INPUT_DEAD, 0);
    recomp_store32(rt, player + GE_INPUT_WATCH_STATE, 0);
    recomp_store32(rt, player + GE_FOV_NATIVE_FOVY, bits(60));
    recomp_store32(rt, player + GE_FOV_ZOOM_FOVY, bits(30));
}
int goldeneye_check_fov(void) {
    Runtime rt;
    check(recomp_runtime_init(&rt, 0x400000, NULL, 0, NULL, 1), "GoldenEye FOV: runtime init");
    gameplay(&rt);
    const char *defaults[] = {NULL, "", "0"};
    for (unsigned i = 0; i < 3; i++) {
        check(goldeneye_fov_configure(defaults[i]), "GoldenEye FOV: default parse");
        MipsContext ctx = arguments(60, GE_FOV_RENDER_RETURN);
        call(&rt, &ctx, true);
    }
    const char *invalid[] = {"nan", "inf", "59", "121", "100junk", "-1", "1e999", " "};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        check(!goldeneye_fov_configure(invalid[i]), "GoldenEye FOV: invalid setting accepted");
    const char *settings[] = {"60", "90", "120"};
    const double horizontal[] = {60, 90, 120};
    const double radians = 3.14159265358979323846 / 180.0;
    const uint32_t callers[] = {GE_FOV_RENDER_RETURN, GE_FOV_VIEWPORT_RETURN, GE_FOV_ZOOM_RETURN};
    for (unsigned i = 0; i < 3; i++) {
        check(goldeneye_fov_configure(settings[i]), "GoldenEye FOV: custom parse");
        MipsContext ctx = arguments(60, GE_FOV_RENDER_RETURN);
        call(&rt, &ctx, false);
        double base_tangent = tan(observed * radians / 2);
        check(fabs(2 * atan(base_tangent * 4 / 3) / radians - horizontal[i]) < 0.00002,
              "GoldenEye FOV: requested horizontal angle");
        for (unsigned j = 0; j < 3; j++) {
            float native = 60.0f / (j + 1);
            ctx = arguments(native, callers[j]);
            call(&rt, &ctx, false);
            check(fabs(tan(observed * radians / 2) / base_tangent -
                       tan(native * radians / 2) / tan(60 * radians / 2)) < 0.000002,
                  "GoldenEye FOV: native zoom magnification changed");
        }
        // Repeated raw updates do not compound; native zoom state stays intact.
        ctx = arguments(60, GE_FOV_RENDER_RETURN);
        call(&rt, &ctx, false);
        check(fabs(tan(observed * radians / 2) - base_tangent) < 0.000002 &&
                  recomp_load32(&rt, player + GE_FOV_NATIVE_FOVY) == bits(60) &&
                  recomp_load32(&rt, player + GE_FOV_ZOOM_FOVY) == bits(30),
              "GoldenEye FOV: override compounded or native zoom fields changed");
        ctx = arguments(60, 0x70000008);
        call(&rt, &ctx, true); // Menus/unrelated calls retain native FOV.
    }
    // Each gate must retain the original argument and still call the setter.
    const uint32_t addresses[] = {player + GE_INPUT_WATCH_STATE, player + GE_INPUT_DEAD,
        GE_INPUT_CONTROLS_LOCKED, GE_INPUT_CAMERA, GE_INPUT_TIMER_ACTIVE,
        GE_INPUT_PLAYERS + 4, GE_FOV_STAGE_ADDRESS, GE_INPUT_CURRENT_PLAYER};
    const uint32_t values[] = {1, 1, 1, 1, 0, player + 0x4000, GE_FOV_TITLE_STAGE, 0x803ffffc};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        gameplay(&rt);
        recomp_store32(&rt, addresses[i], values[i]);
        MipsContext ctx = arguments(60, GE_FOV_RENDER_RETURN);
        call(&rt, &ctx, true);
    }
    check(goldeneye_fov_configure(NULL), "GoldenEye FOV: reset");
    recomp_runtime_destroy(&rt);
    puts("PASS: GoldenEye FOV defaults, ranges, call scope, zoom ratios, repeated updates and state gates (synthetic)");
    return 0;
}
