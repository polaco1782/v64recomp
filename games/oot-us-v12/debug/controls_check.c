/* Synthetic adapter boundaries; live movement and geometry acceptance is separate. */
#define _POSIX_C_SOURCE 200809L
#include "controls.h"
#include "oot_controls_config.h"
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oot_play_update_hook(Runtime *, MipsContext *, RecompFunction);
void oot_camera_update_hook(Runtime *, MipsContext *, RecompFunction);
void oot_normal_camera_hook(Runtime *, MipsContext *, RecompFunction);
static const uint32_t state = 0x80200000, camera = 0x802001e0;
static unsigned collision_calls, original_calls;
static bool run_normal;
static float native_fov;
static void check(bool ok, const char *message) { recomp_game_require(ok, message, 0); }
static float load_float(Runtime *rt, uint32_t address) {
    uint32_t bits = recomp_load32(rt, address); float f; memcpy(&f, &bits, 4); return f;
}
static void store_float(Runtime *rt, uint32_t address, float f) {
    uint32_t bits; memcpy(&bits, &f, 4); recomp_store32(rt, address, bits);
}
static void collision(Runtime *rt, MipsContext *ctx) {
    check((uint32_t)ctx->gpr[4] == camera && (uint32_t)ctx->gpr[5] == camera + OOT_CTL_AT &&
          (uint32_t)ctx->gpr[6] == camera + OOT_CTL_EYE, "OoT check: original collision ABI");
    (void)rt; collision_calls++; ctx->gpr[2] = 0xdeadbeef;
}
static void normal(Runtime *rt, MipsContext *ctx) {
    original_calls++;
    for (unsigned i = 0; i < 3; i++) {
        store_float(rt, camera + OOT_CTL_AT + i * 4, 0);
        store_float(rt, camera + OOT_CTL_EYE_NEXT + i * 4, i == 2 ? 200.0f : 0);
        store_float(rt, camera + OOT_CTL_EYE + i * 4, i == 2 ? 200.0f : 0);
    }
    store_float(rt, camera + OOT_CTL_DISTANCE, 200);
    ctx->gpr[2] = 1;
}
static void update_camera(Runtime *rt, MipsContext *ctx) {
    original_calls++;
    if (run_normal) {
        MipsContext nested = *ctx;
        nested.gpr[4] = camera; nested.gpr[31] = OOT_CTL_DISPATCH_RETURN;
        oot_normal_camera_hook(rt, &nested, normal);
        check(nested.gpr[2] == 1, "OoT check: collision helper clobbered native camera return");
    }
    store_float(rt, state + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV, native_fov);
    ctx->gpr[2] = 99;
}
static void update_play(Runtime *rt, MipsContext *ctx) { (void)rt; ctx->gpr[2] = 123; }
static MipsContext arguments(void) {
    MipsContext ctx = {0};
    ctx.gpr[4] = 0x80300000; ctx.gpr[5] = camera; ctx.gpr[29] = 0x807f0000;
    return ctx;
}
static void gameplay(Runtime *rt) {
    recomp_store32(rt, state + OOT_CTL_MAIN_FIELD, OOT_CTL_PLAY_MAIN);
    recomp_store32(rt, state + OOT_CTL_RUNNING, 1);
    recomp_store32(rt, OOT_CTL_GAME_MODE, 0);
    recomp_store16(rt, OOT_CTL_HEALTH, 48);
    recomp_store16(rt, state + OOT_CTL_PAUSE, 0);
    recomp_store16(rt, state + OOT_CTL_GAME_OVER, 0);
    recomp_store8(rt, state + OOT_CTL_MESSAGE, 0);
    recomp_store8(rt, state + OOT_CTL_CUTSCENE, 0);
    recomp_store8(rt, state + OOT_CTL_TRANSITION, 0);
    recomp_store16(rt, state + OOT_CTL_ACTIVE_CAMERA, 0);
    recomp_store32(rt, state + OOT_CTL_CAMERA_PTRS, camera);
    recomp_store32(rt, camera + OOT_CTL_CAMERA_PLAY, state);
    recomp_store16(rt, camera + OOT_CTL_STATUS, 7);
    recomp_store16(rt, camera + OOT_CTL_SETTING, 1);
    recomp_store16(rt, camera + OOT_CTL_MODE, 0);
    recomp_store16(rt, state + 0x14, 0);
    MipsContext ctx = arguments(); ctx.gpr[4] = state; ctx.gpr[31] = OOT_CTL_PLAY_RETURN;
    oot_play_update_hook(rt, &ctx, update_play);
    check(ctx.gpr[2] == 123, "OoT check: original Play_Update return");
}
int oot_check_controls(void) {
#ifdef _WIN32
    _putenv_s("RECOMP_OOT_MOUSE", "1"); _putenv_s("RECOMP_MOUSE_SENSITIVITY", "0.08");
    _putenv_s("RECOMP_MOUSE_INVERT_Y", "0");
#else
    setenv("RECOMP_OOT_MOUSE", "1", 1); setenv("RECOMP_MOUSE_SENSITIVITY", "0.08", 1);
    setenv("RECOMP_MOUSE_INVERT_Y", "0", 1);
#endif
    const RecompFunctionEntry functions[] = {{OOT_CTL_BG_CHECK, 0x5c, 1, collision, "collision observer"}};
    const bool loaded[] = {true, true};
    Runtime rt;
    check(recomp_runtime_init(&rt, 0x800000, functions, 1, loaded, 2), "OoT check: runtime init");
    oot_controls_init(NULL); gameplay(&rt);
    check(oot_controls_playable(&rt), "OoT check: gameplay capture");
    RecompPad pad = {.connected = true, .buttons = 0xffff};
    oot_controls_submit((RecompMouseFrame){.active = true, .move_x = 1, .move_y = 1,
        .actions = RECOMP_MOUSE_ATTACK | RECOMP_MOUSE_USE | RECOMP_MOUSE_SECONDARY_ATTACK |
                   RECOMP_MOUSE_DEFEND | RECOMP_MOUSE_NEXT_WEAPON | RECOMP_MOUSE_PREVIOUS_WEAPON |
                   RECOMP_MOUSE_STATUS}, &pad);
    check(pad.stick_x == 80 && pad.stick_y == 80 && pad.buttons == 0xf017,
          "OoT check: WASD, action, target, shield and C-item mapping");
    oot_controls_submit((RecompMouseFrame){.active = true, .move_y = -1,
        .actions = RECOMP_MOUSE_SPEED | RECOMP_MOUSE_STATUS}, &pad);
    check(!pad.stick_x && pad.stick_y == -40 && pad.buttons == 0x1008,
          "OoT check: walk modifier / C-up");
    native_fov = 60;
    const char *defaults[] = {NULL, "", "0"};
    for (unsigned i = 0; i < 3; i++) {
        check(oot_fov_configure(defaults[i]), "OoT check: default FOV parse");
        MipsContext ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
        check(ctx.gpr[2] == 99 && load_float(&rt, state + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV) == 60,
              "OoT check: native FOV / hidden-return ABI");
    }
    const char *settings[] = {"60", "110", "120"};
    const double horizontal[] = {60, 110, 120}, radians = 3.14159265358979323846 / 180;
    for (unsigned i = 0; i < 3; i++) {
        check(oot_fov_configure(settings[i]), "OoT check: FOV parse");
        MipsContext ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
        double v = load_float(&rt, state + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV);
        check(fabs(2 * atan(tan(v * radians / 2) * 4 / 3) / radians - horizontal[i]) < 0.00002,
              "OoT check: horizontal FOV conversion");
        native_fov = 30; ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
        double zoom = load_float(&rt, state + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV);
        check(fabs(tan(zoom * radians / 2) / tan(v * radians / 2) - tan(15 * radians) / tan(30 * radians)) < 0.000002,
              "OoT check: native zoom ratio");
        native_fov = 60;
    }
    const char *invalid[] = {"nan", "inf", "59", "121", "100junk", "-1", "1e999"};
    for (unsigned i = 0; i < 7; i++) check(!oot_fov_configure(invalid[i]), "OoT check: invalid FOV accepted");
    check(oot_fov_configure("110"), "OoT check: FOV reset");
    const uint32_t fields[] = {state + OOT_CTL_PAUSE, state + OOT_CTL_GAME_OVER, state + OOT_CTL_MESSAGE,
        state + OOT_CTL_CUTSCENE, state + OOT_CTL_TRANSITION, OOT_CTL_GAME_MODE, OOT_CTL_HEALTH,
        state + OOT_CTL_ACTIVE_CAMERA, state + OOT_CTL_RUNNING};
    const unsigned sizes[] = {2, 2, 1, 1, 1, 4, 2, 2, 4};
    for (unsigned i = 0; i < 9; i++) {
        gameplay(&rt); recomp_store(&rt, fields[i], i == 6 || i == 8 ? 0 : 1, sizes[i]);
        check(!oot_controls_playable(&rt), "OoT check: pause/dialog/cutscene/death/transition gate");
        MipsContext ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
        check(load_float(&rt, state + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV) == 60, "OoT check: excluded FOV changed");
    }
    gameplay(&rt); recomp_store16(&rt, camera + OOT_CTL_SETTING, 0x19);
    MipsContext ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
    check(load_float(&rt, state + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV) == 60, "OoT check: fixed-background FOV changed");
    gameplay(&rt); run_normal = true; original_calls = collision_calls = 0;
    oot_controls_submit((RecompMouseFrame){.active = true, .dx = 1125}, &pad);
    ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
    check(original_calls == 2 && collision_calls == 1 && fabs(load_float(&rt, camera + OOT_CTL_EYE) + 200) < 0.001,
          "OoT check: normal camera orbit / original collision");
    ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
    check(collision_calls == 2 && fabs(load_float(&rt, camera + OOT_CTL_EYE) + 200) < 0.001,
          "OoT check: consumed delta / persistent orbit");
    oot_controls_submit((RecompMouseFrame){.active = true, .dx = 100,
        .actions = RECOMP_MOUSE_SECONDARY_ATTACK}, &pad);
    ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
    check(collision_calls == 2 && load_float(&rt, camera + OOT_CTL_EYE + 8) == 200,
          "OoT check: Z-targeting retains native camera");
    oot_controls_submit((RecompMouseFrame){0}, &pad);
    oot_controls_submit((RecompMouseFrame){.active = true}, &pad);
    ctx = arguments(); oot_camera_update_hook(&rt, &ctx, update_camera);
    check(collision_calls == 2, "OoT check: stale motion after release");
    recomp_runtime_destroy(&rt);
    puts("PASS: OoT WASD/actions, orbit/collision ABI, targeting, FOV/zoom and state gates (synthetic)");
    return 0;
}
