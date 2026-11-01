/* Actual translated SDK matrix checks; sector-window checks use an observer.
 * Gameplay, camera tilt and portal traversal still require in-game acceptance. */
#include "fov.h"
#include "game.h"
#include "generated.h"
#include "duke_config.h"
#include "duke_fov_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void duke_fov_projection_hook(Runtime *, MipsContext *, RecompFunction);
void duke_fov_visibility_hook(Runtime *, MipsContext *, RecompFunction);
void duke_fov_sprite_hook(Runtime *, MipsContext *, RecompFunction);
static const uint32_t matrix = 0x80300000;
static unsigned calls;
static float seen_low, seen_high;
static float seen_width, seen_height;
static void observe_sprite(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    calls++;
    uint32_t width = (uint32_t)ctx->gpr[6], height = (uint32_t)ctx->gpr[7];
    memcpy(&seen_width, &width, sizeof(width));
    memcpy(&seen_height, &height, sizeof(height));
}
static void check(bool ok, const char *message) { recomp_game_require(ok, message, 0); }
static uint32_t bits(float value) {
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}
static float from_bits(uint32_t word) {
    float value;
    memcpy(&value, &word, sizeof(value));
    return value;
}
static MipsContext arguments(Runtime *rt, uint32_t caller) {
    MipsContext ctx = {0};
    ctx.gpr[4] = matrix;
    ctx.gpr[5] = matrix + 0x100;
    ctx.gpr[6] = DUKE_FOV_VERTICAL_BITS;
    ctx.gpr[7] = DUKE_FOV_ASPECT_BITS;
    ctx.gpr[29] = 0x807f0000;
    ctx.gpr[31] = caller;
    recomp_store32(rt, 0x807f0010, bits(5));
    recomp_store32(rt, 0x807f0014, bits(16384));
    recomp_store32(rt, 0x807f0018, bits(1));
    return ctx;
}
static void matrix_bytes(Runtime *rt, uint8_t output[64]) {
    for (unsigned i = 0; i < 64; i++) output[i] = recomp_load8(rt, matrix + i);
}
static double fixed_x(Runtime *rt) {
    return (int16_t)recomp_load16(rt, matrix) + recomp_load16(rt, matrix + 32) / 65536.0;
}
static void observe(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    calls++;
    seen_low = from_bits(ctx->fpr[12]);
    seen_high = from_bits(ctx->fpr[14]);
    ctx->gpr[2] = 123;
}
int duke_check_fov(const RecompGameOptions *options) {
    Runtime rt;
    check(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                             recomp_boot_sections, recomp_section_count), "Duke FOV: runtime init");
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    recomp_store16(&rt, DUKE_FOV_PLAYERS, 1);
    check(duke_fov_configure(NULL), "Duke FOV: initial reset");
    uint8_t baseline_float[64], baseline_fixed[64], observed[64];
    MipsContext ctx = arguments(&rt, DUKE_FOV_CPU_RETURN);
    recomp_s0_800c3130(&rt, &ctx);
    matrix_bytes(&rt, baseline_float);
    double original_x = from_bits(recomp_load32(&rt, matrix));
    ctx = arguments(&rt, DUKE_FOV_GPU_RETURN);
    recomp_s0_800c3360(&rt, &ctx);
    matrix_bytes(&rt, baseline_fixed);
    const char *defaults[] = {NULL, "", "0"};
    for (unsigned i = 0; i < 3; i++) {
        check(duke_fov_configure(defaults[i]), "Duke FOV: default parse");
        ctx = arguments(&rt, DUKE_FOV_CPU_RETURN);
        duke_fov_projection_hook(&rt, &ctx, recomp_s0_800c3130);
        matrix_bytes(&rt, observed);
        check(!memcmp(observed, baseline_float, 64), "Duke FOV: stock CPU projection changed");
        ctx = arguments(&rt, DUKE_FOV_GPU_RETURN);
        recomp_s0_800c3360(&rt, &ctx);
        matrix_bytes(&rt, observed);
        check(!memcmp(observed, baseline_fixed, 64), "Duke FOV: stock GPU projection changed");
    }
    const char *invalid[] = {"nan", "inf", "59", "121", "100junk", "-1", "1e999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        check(!duke_fov_configure(invalid[i]), "Duke FOV: invalid setting accepted");
    const char *settings[] = {"60", "100", "110", "120"};
    const double horizontal[] = {60, 100, 110, 120};
    for (unsigned i = 0; i < 4; i++) {
        check(duke_fov_configure(settings[i]), "Duke FOV: custom parse");
        double expected = 1 / tan(horizontal[i] * 3.14159265358979323846 / 360);
        for (unsigned repeat = 0; repeat < 2; repeat++) {
            ctx = arguments(&rt, DUKE_FOV_CPU_RETURN);
            duke_fov_projection_hook(&rt, &ctx, recomp_s0_800c3130);
            check(fabs(from_bits(recomp_load32(&rt, matrix)) - expected) < 0.00001,
                  "Duke FOV: translated CPU matrix angle");
            ctx = arguments(&rt, DUKE_FOV_GPU_RETURN);
            recomp_s0_800c3360(&rt, &ctx); // Includes the generated F wrapper and F2L conversion.
            check(fabs(fixed_x(&rt) - expected) < 0.00004, "Duke FOV: translated GPU matrix angle");
        }
        ctx = arguments(&rt, 0x80000008);
        duke_fov_projection_hook(&rt, &ctx, recomp_s0_800c3130);
        matrix_bytes(&rt, observed);
        check(!memcmp(observed, baseline_float, 64), "Duke FOV: unrelated float projection changed");
        ctx = arguments(&rt, 0x80000008);
        recomp_s0_800c3360(&rt, &ctx);
        matrix_bytes(&rt, observed);
        check(!memcmp(observed, baseline_fixed, 64), "Duke FOV: unrelated fixed projection changed");
        const uint32_t sprite_callers[] = {DUKE_FOV_SPRITE_RETURN, DUKE_FOV_EFFECT_RETURN, 0x80066708};
        for (unsigned j = 0; j < 3; j++) {
            ctx = arguments(&rt, sprite_callers[j]);
            ctx.gpr[6] = bits(80);
            ctx.gpr[7] = bits(120);
            ctx.fpr[12] = bits(160); ctx.fpr[14] = bits(100);
            calls = 0;
            duke_fov_sprite_hook(&rt, &ctx, observe_sprite);
            double ratio = j < 2 ? expected / original_x : 1;
            check(calls == 1 && fabs(seen_width - 80 * ratio) < 0.0001 &&
                      fabs(seen_height - 120 * ratio) < 0.0001 &&
                      ctx.fpr[12] == bits(160) && ctx.fpr[14] == bits(100),
                  "Duke FOV: world billboard size must track matrix focal length / preserve HUD");
        }
        // Preserve each native pitch allowance; change only the initial window.
        for (unsigned pitch = 0; pitch < 2; pitch++) {
            float half = 35.0f + pitch * 20.0f;
            ctx = (MipsContext){0};
            ctx.gpr[31] = DUKE_FOV_SCAN_RETURN;
            ctx.fpr[12] = bits(355 - half);
            ctx.fpr[14] = bits(355 + half);
            calls = 0;
            duke_fov_visibility_hook(&rt, &ctx, observe);
            check(calls == 1 && ctx.gpr[2] == 123 && fabs((seen_low + seen_high) / 2 - 355) < 0.0001 &&
                      (seen_high - seen_low) / 2 >= fmax(half, horizontal[i] / 2 + 1 + pitch * 20),
                  "Duke FOV: visibility window/original call");
            ctx.gpr[31] = 0x80059c8c; // Native recursive scan, not the initial drawrooms call.
            ctx.fpr[12] = bits(310);
            ctx.fpr[14] = bits(380);
            duke_fov_visibility_hook(&rt, &ctx, observe);
            check(seen_low == 310 && seen_high == 380, "Duke FOV: recursive portal clipping changed");
        }
    }
    recomp_store16(&rt, DUKE_FOV_PLAYERS, 2);
    ctx = arguments(&rt, DUKE_FOV_CPU_RETURN);
    duke_fov_projection_hook(&rt, &ctx, recomp_s0_800c3130);
    matrix_bytes(&rt, observed);
    check(!memcmp(observed, baseline_float, 64), "Duke FOV: multiplayer CPU projection changed");
    ctx = arguments(&rt, DUKE_FOV_GPU_RETURN);
    recomp_s0_800c3360(&rt, &ctx);
    matrix_bytes(&rt, observed);
    check(!memcmp(observed, baseline_fixed, 64), "Duke FOV: multiplayer GPU projection changed");
    ctx = (MipsContext){0};
    ctx.gpr[31] = DUKE_FOV_SCAN_RETURN;
    ctx.fpr[12] = bits(0);
    ctx.fpr[14] = bits(70);
    duke_fov_visibility_hook(&rt, &ctx, observe);
    check(seen_low == 0 && seen_high == 70, "Duke FOV: multiplayer visibility changed");
    check(duke_fov_configure(NULL), "Duke FOV: final reset");
    free(rom);
    recomp_runtime_destroy(&rt);
    puts("PASS: Duke stock/custom translated CPU/GPU projections, scope, billboard focal scale and synthetic visibility windows");
    return 0;
}
