/* Checks the actual translated perspective math; gameplay is tested separately. */
#include "fov.h"
#include "game.h"
#include "generated.h"
#include "hexen_config.h"
#include "hexen_fov_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hexen_fov_projection_hook(Runtime *, MipsContext *, RecompFunction);
void hexen_fov_culling_hook(Runtime *, MipsContext *, RecompFunction);
static unsigned calls;
static uint32_t seen_cull;
static void check(bool condition, const char *message) { recomp_game_require(condition, message, 0); }
static void observe_cull(Runtime *rt, MipsContext *ctx) {
    (void)ctx;
    calls++;
    seen_cull = recomp_load32(rt, HEXEN_FOV_CULL_ANGLE);
}
static uint32_t bits(float value) {
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}
static MipsContext arguments(Runtime *rt) {
    MipsContext ctx = {0};
    ctx.gpr[4] = HEXEN_FOV_PROJECTION_MATRIX;
    ctx.gpr[5] = 0x80084c34;
    ctx.gpr[6] = HEXEN_FOV_VERTICAL_BITS;
    ctx.gpr[7] = HEXEN_FOV_ASPECT_BITS;
    ctx.gpr[29] = 0x803f0000;
    ctx.gpr[31] = HEXEN_FOV_PROJECTION_RETURN;
    recomp_store32(rt, 0x803f0010, bits(1));
    recomp_store32(rt, 0x803f0014, bits(5000));
    recomp_store32(rt, 0x803f0018, bits(1));
    return ctx;
}
static void matrix_bytes(Runtime *rt, uint32_t matrix, uint8_t output[64]) {
    for (unsigned i = 0; i < 64; i++) output[i] = recomp_load8(rt, matrix + i);
}
static double element(Runtime *rt, unsigned index) {
    uint32_t m = HEXEN_FOV_PROJECTION_MATRIX;
    return (int16_t)recomp_load16(rt, m + index * 2) +
           recomp_load16(rt, m + 32 + index * 2) / 65536.0;
}
int hexen_check_fov(const RecompGameOptions *options) {
    Runtime rt;
    check(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                             recomp_boot_sections, recomp_section_count), "Hexen FOV check: runtime init");
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    uint8_t baseline[64], observed[64];
    MipsContext ctx = arguments(&rt);
    recomp_s0_800559f0(&rt, &ctx);
    matrix_bytes(&rt, HEXEN_FOV_PROJECTION_MATRIX, baseline);
    double original_x = element(&rt, 0);
    const char *defaults[] = {NULL, "", "0"};
    for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        check(hexen_fov_configure(defaults[i]), "Hexen FOV check: default parse");
        ctx = arguments(&rt);
        hexen_fov_projection_hook(&rt, &ctx, recomp_s0_800559f0);
        matrix_bytes(&rt, HEXEN_FOV_PROJECTION_MATRIX, observed);
        check(!memcmp(baseline, observed, 64), "Hexen FOV check: default matrix changed");
    }
    const char *invalid[] = {"nan", "inf", "59", "121", "100junk", "-1", "1e999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        check(!hexen_fov_configure(invalid[i]), "Hexen FOV check: invalid setting accepted");
    const char *settings[] = {"60", "100", "120"};
    const double horizontal[] = {60, 100, 120};
    for (unsigned i = 0; i < 3; i++) {
        check(hexen_fov_configure(settings[i]), "Hexen FOV check: custom parse");
        ctx = arguments(&rt);
        hexen_fov_projection_hook(&rt, &ctx, recomp_s0_800559f0);
        double expected = 1 / tan(horizontal[i] * 3.14159265358979323846 / 360);
        check(fabs(element(&rt, 0) - expected) < 0.00004,
              "Hexen FOV check: translated projection has wrong horizontal FOV");
        check((horizontal[i] < 94 ? element(&rt, 0) > original_x : element(&rt, 0) < original_x),
              "Hexen FOV check: projection zoom direction");
        // An unrelated perspective call must retain its original projection.
        ctx = arguments(&rt);
        ctx.gpr[31] = 0x803bb388;
        hexen_fov_projection_hook(&rt, &ctx, recomp_s0_800559f0);
        matrix_bytes(&rt, HEXEN_FOV_PROJECTION_MATRIX, observed);
        check(!memcmp(baseline, observed, 64), "Hexen FOV check: intro projection changed");
        // Observe the scalar supplied to the original visibility initializer,
        // including restoration and exactly-once execution of its side effects.
        recomp_store32(&rt, HEXEN_FOV_CULL_ANGLE, HEXEN_FOV_CULL_BITS);
        calls = 0;
        hexen_fov_culling_hook(&rt, &ctx, observe_cull);
        float fine;
        memcpy(&fine, &seen_cull, sizeof(fine));
        check(calls == 1 && recomp_load32(&rt, HEXEN_FOV_CULL_ANGLE) == HEXEN_FOV_CULL_BITS,
              "Hexen FOV check: visibility original/restoration");
        check(fine * 360 / HEXEN_FOV_FINE_ANGLES > horizontal[i] / 2,
              "Hexen FOV check: visibility narrower than projection");
        check(fine * 360 / HEXEN_FOV_FINE_ANGLES < atan(2) * 180 / 3.14159265358979323846,
              "Hexen FOV check: native tangent cutoff exceeded");
    }
    check(hexen_fov_configure(NULL), "Hexen FOV check: reset");
    free(rom);
    recomp_runtime_destroy(&rt);
    puts("PASS: Hexen original/custom translated projections, scope, FOV parsing and visibility span/restoration");
    return 0;
}
