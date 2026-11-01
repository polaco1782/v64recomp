/* Executes the translated command builder; physics/menu acceptance is separate. */
#define _POSIX_C_SOURCE 200809L
#include "duke_input.h"
#include "duke_input_config.h"
#include "duke_config.h"
#include "generated.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>

void duke_input_command_hook(Runtime *, MipsContext *, RecompFunction);
void duke_input_process_hook(Runtime *, MipsContext *, RecompFunction);
static unsigned calls;
static void check(bool ok, const char *message) { recomp_game_require(ok, message, 0); }
static void observe(Runtime *rt, MipsContext *ctx) { (void)rt; calls++; ctx->gpr[2] = 123; }
static void process(Runtime *rt, MipsContext *ctx) {
    check(recomp_load8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE) == 1 &&
          !recomp_load8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_CENTERING) &&
          !recomp_load8(rt, DUKE_INPUT_SYNC + 1), "Duke input: native free-look setup");
    observe(rt, ctx);
}
static void gameplay(Runtime *rt) {
    recomp_store8(rt, DUKE_INPUT_GAME_STATE, 2);
    recomp_store8(rt, DUKE_INPUT_INACTIVE, 0);
    recomp_store16(rt, DUKE_INPUT_PLAYERS, 1);
    recomp_store16(rt, DUKE_INPUT_USER + DUKE_INPUT_PAUSED, 0);
    recomp_store8(rt, DUKE_INPUT_USER + DUKE_INPUT_OVERHEAD, 0);
    recomp_store16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_HEALTH, 100);
    recomp_store16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_NEW_OWNER, 0xffff);
    recomp_store16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_FIST, 0);
    recomp_store16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW, 0);
    recomp_store32(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_PITCH, 100);
    recomp_store8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE, 0);
}
static MipsContext arguments(void) {
    MipsContext ctx = {0};
    ctx.gpr[29] = 0x807f0000;
    ctx.gpr[31] = DUKE_INPUT_COMMAND_RETURN;
    return ctx;
}
static void command(Runtime *rt, RecompMouseFrame frame) {
    duke_input_submit(frame);
    MipsContext ctx = arguments();
    duke_input_command_hook(rt, &ctx, recomp_s0_800a2754);
}
int duke_check_input(const RecompGameOptions *options) {
    Runtime rt;
    check(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                             recomp_boot_sections, recomp_section_count), "Duke input: runtime init");
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
#ifdef _WIN32
    _putenv_s("RECOMP_DUKE_MOUSE", "1");
    _putenv_s("RECOMP_MOUSE_SENSITIVITY", "0.08");
    _putenv_s("RECOMP_MOUSE_INVERT_Y", "0");
#else
    setenv("RECOMP_DUKE_MOUSE", "1", 1);
    setenv("RECOMP_MOUSE_SENSITIVITY", "0.08", 1);
    setenv("RECOMP_MOUSE_INVERT_Y", "0", 1);
#endif
    duke_input_init(NULL);
    gameplay(&rt);
    check(duke_input_playable(&rt), "Duke input: single-player gate");
    for (unsigned i = 0; i < 16; i++) recomp_store32(&rt, DUKE_INPUT_CONTROL_MAP + i * 4, 0x20000000 + i);
    recomp_store16(&rt, DUKE_INPUT_CONTROL_BUTTONS, 0x1234);
    recomp_store16(&rt, DUKE_INPUT_CONTROL_X, 25);
    recomp_store16(&rt, DUKE_INPUT_CONTROL_Y, 30);
    for (unsigned i = 0; i < 4; i++) recomp_store16(&rt, DUKE_INPUT_WORK_CONTROLS + i * 2, (uint16_t)(111 + i));
    command(&rt, (RecompMouseFrame){.dx = 562.5, .move_y = 1, .active = true,
        .actions = RECOMP_MOUSE_ATTACK | RECOMP_MOUSE_JUMP | RECOMP_MOUSE_USE});
    check(recomp_load16(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW) == 256,
          "Duke input: horizontal degrees / 2048-unit angle conversion");
    check(!recomp_load8(&rt, DUKE_INPUT_COMMAND) &&
          recomp_load32(&rt, DUKE_INPUT_COMMAND + 8) == ((1u << 29) | 5),
          "Duke input: translated action command / no analog turning");
    int16_t forward = (int16_t)recomp_load16(&rt, DUKE_INPUT_COMMAND + 2);
    int16_t side = (int16_t)recomp_load16(&rt, DUKE_INPUT_COMMAND + 4);
    check(forward > 0 && side > 0 && abs(forward - side) <= 1, "Duke input: movement follows mouse yaw");
    for (unsigned i = 0; i < 16; i++)
        check(recomp_load32(&rt, DUKE_INPUT_CONTROL_MAP + i * 4) == 0x20000000 + i,
              "Duke input: control-map restoration");
    check(recomp_load16(&rt, DUKE_INPUT_CONTROL_BUTTONS) == 0x1234 &&
          recomp_load16(&rt, DUKE_INPUT_CONTROL_X) == 25 && recomp_load16(&rt, DUKE_INPUT_CONTROL_Y) == 30 &&
          !recomp_load8(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE), "Duke input: native controls restoration");
    for (unsigned i = 0; i < 4; i++)
        check(recomp_load16(&rt, DUKE_INPUT_WORK_CONTROLS + i * 2) == 111 + i, "Duke input: work controls restoration");
    MipsContext ctx = arguments();
    ctx.gpr[31] = DUKE_INPUT_PROCESS_RETURN;
    recomp_store8(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_CENTERING, 9);
    recomp_store8(&rt, DUKE_INPUT_SYNC + 1, 55);
    calls = 0;
    duke_input_process_hook(&rt, &ctx, process);
    check(calls == 1 && ctx.gpr[2] == 123 && !recomp_load8(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE),
          "Duke input: process original call / aim restoration");
    // A second consume keeps held movement, but the mouse delta is gone.
    ctx = arguments();
    duke_input_command_hook(&rt, &ctx, recomp_s0_800a2754);
    check(recomp_load16(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW) == 256 &&
          (int16_t)recomp_load16(&rt, DUKE_INPUT_COMMAND + 2) == forward,
          "Duke input: delta consumed once / held movement");
    for (int axis = -1; axis <= 1; axis += 2) {
        command(&rt, (RecompMouseFrame){.move_x = (int8_t)axis, .active = true});
        check((int16_t)recomp_load16(&rt, DUKE_INPUT_COMMAND + 2) * axis < 0 &&
              (int16_t)recomp_load16(&rt, DUKE_INPUT_COMMAND + 4) * axis > 0,
              "Duke input: native strafe direction");
    }
    command(&rt, (RecompMouseFrame){.dy = -100000, .active = true});
    check((int32_t)recomp_load32(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_PITCH) == 228, "Duke input: look-up limit");
    command(&rt, (RecompMouseFrame){.dy = 100000, .active = true});
    check((int32_t)recomp_load32(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_PITCH) == -28, "Duke input: look-down limit");
    command(&rt, (RecompMouseFrame){.wheel = -2, .active = true});
    check((recomp_load32(&rt, DUKE_INPUT_COMMAND + 8) & 0xf00) == 0xc00, "Duke input: next weapon detent");
    ctx = arguments(); duke_input_command_hook(&rt, &ctx, recomp_s0_800a2754);
    check(!(recomp_load32(&rt, DUKE_INPUT_COMMAND + 8) & 0xf00), "Duke input: weapon release gap");
    ctx = arguments(); duke_input_command_hook(&rt, &ctx, recomp_s0_800a2754);
    check((recomp_load32(&rt, DUKE_INPUT_COMMAND + 8) & 0xf00) == 0xc00, "Duke input: queued weapon detent");
    // State gates clear pending deltas and still call the original once.
    const uint32_t addresses[] = {DUKE_INPUT_GAME_STATE, DUKE_INPUT_INACTIVE, DUKE_INPUT_PLAYERS,
        DUKE_INPUT_USER + DUKE_INPUT_PAUSED, DUKE_INPUT_USER + DUKE_INPUT_OVERHEAD,
        DUKE_INPUT_PLAYER + DUKE_INPUT_HEALTH, DUKE_INPUT_PLAYER + DUKE_INPUT_FIST,
        DUKE_INPUT_PLAYER + DUKE_INPUT_NEW_OWNER};
    const unsigned widths[] = {1, 1, 2, 2, 1, 2, 2, 2};
    const uint32_t values[] = {3, 1, 2, 1, 1, 0, 1, 0};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        gameplay(&rt);
        recomp_store(&rt, addresses[i], values[i], widths[i]);
        check(!duke_input_playable(&rt), "Duke input: state gate");
        duke_input_submit((RecompMouseFrame){.dx = 500, .active = true});
        ctx = arguments(); calls = 0;
        duke_input_command_hook(&rt, &ctx, observe);
        check(calls == 1 && !recomp_load16(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW), "Duke input: gated original");
    }
    gameplay(&rt);
    duke_input_submit((RecompMouseFrame){.dx = 500, .active = true});
    duke_input_submit((RecompMouseFrame){0});
    command(&rt, (RecompMouseFrame){.active = true});
    check(!recomp_load16(&rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW), "Duke input: capture-loss stale motion");
    free(rom);
    recomp_runtime_destroy(&rt);
    puts("PASS: Duke translated movement/actions, mouse axes/limits, consumed deltas, weapon queue, restoration and state gates");
    return 0;
}
