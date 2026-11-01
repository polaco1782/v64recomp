/* A synthetic command boundary check; this does not establish live gameplay. */
#include "hexen_input.h"
#include "hexen_input_config.h"
#include "game.h"
#include <stdio.h>

void hexen_input_player_hook(Runtime *, MipsContext *, RecompFunction);
static unsigned calls;
static void original(Runtime *rt, MipsContext *ctx) { (void)rt; (void)ctx; calls++; }
static void check(bool condition, const char *message) { recomp_game_require(condition, message, 0); }
static void update(Runtime *rt, MipsContext *ctx, RecompMouseFrame frame) {
    RecompPad pad = {.connected = true, .buttons = 0xffff};
    hexen_input_submit(frame, &pad);
    if (frame.active) check(pad.buttons == 0x1000, "Hexen check: FPS pad bits leaked");
    unsigned before = calls;
    hexen_input_player_hook(rt, ctx, original);
    check(calls == before + 1, "Hexen check: original call count");
}
int hexen_check_input(void) {
    Runtime rt;
    check(recomp_runtime_init(&rt, 0x400000, NULL, 0, NULL, 1), "Hexen check: runtime init");
    hexen_input_init(NULL);
    check(hexen_input_enabled(), "Hexen check: unset replay/disable environment variables");
    uint32_t p = HEXEN_INPUT_PLAYERS, c = p + HEXEN_INPUT_COMMAND;
    MipsContext ctx = {0};
    ctx.gpr[4] = p;
    ctx.gpr[31] = HEXEN_INPUT_PLAYER_RETURN;
    recomp_store32(&rt, p, 0x80200000);
    recomp_store32(&rt, HEXEN_INPUT_PLAYER_COUNT, 1);
    recomp_store16(&rt, HEXEN_INPUT_PLAYERINGAME, 1);
    const int forward[] = {29, 60, 25, 50, 22, 46, 24, 49};
    const int side[] = {27, 59, 24, 40, 21, 37, 23, 39};
    for (unsigned i = 0; i < 8; i++) {
        recomp_store32(&rt, HEXEN_INPUT_FORWARD_TABLE + i * 4, forward[i]);
        recomp_store32(&rt, HEXEN_INPUT_SIDE_TABLE + i * 4, side[i]);
    }
    check(hexen_input_playable(&rt), "Hexen check: playable gate");
    for (unsigned cls = 0; cls < 4; cls++) for (unsigned run = 0; run < 2; run++) {
        recomp_store16(&rt, p + HEXEN_INPUT_CLASS, (uint16_t)cls);
        update(&rt, &ctx, (RecompMouseFrame){.active = true, .move_y = -1, .move_x = 1,
               .actions = RECOMP_MOUSE_ATTACK | RECOMP_MOUSE_USE | RECOMP_MOUSE_JUMP |
                          (run ? RECOMP_MOUSE_SPEED : 0)});
        check((int8_t)recomp_load8(&rt, c) == -forward[cls * 2 + run] &&
              recomp_load8(&rt, c + 1) == side[cls * 2 + run], "Hexen check: class/run movement");
        check(recomp_load8(&rt, c + 4) == 3 && recomp_load8(&rt, c + 6) == 0x80,
              "Hexen check: attack/use/jump");
    }
    update(&rt, &ctx, (RecompMouseFrame){.active = true, .dx = 100, .dy = 100});
    int16_t turn = (int16_t)recomp_load16(&rt, c + 2);
    check(turn < 0 && (int16_t)recomp_load16(&rt, p + HEXEN_INPUT_LOOKDIR) < 0,
          "Hexen check: look direction");
    int16_t pitch = (int16_t)recomp_load16(&rt, p + HEXEN_INPUT_LOOKDIR);
    hexen_input_player_hook(&rt, &ctx, original);
    check(!recomp_load16(&rt, c + 2) && (int16_t)recomp_load16(&rt, p + HEXEN_INPUT_LOOKDIR) == pitch,
          "Hexen check: motion consumed more than once");
    update(&rt, &ctx, (RecompMouseFrame){.active = true, .dy = 100000});
    check((int16_t)recomp_load16(&rt, p + HEXEN_INPUT_LOOKDIR) == -110, "Hexen check: lower pitch clamp");
    update(&rt, &ctx, (RecompMouseFrame){.active = true, .dy = -100000});
    check(recomp_load16(&rt, p + HEXEN_INPUT_LOOKDIR) == 90, "Hexen check: upper pitch clamp");
    update(&rt, &ctx, (RecompMouseFrame){.active = true, .wheel = -2});
    check(recomp_load8(&rt, c + 4) == 0xc, "Hexen check: wheel next");
    hexen_input_player_hook(&rt, &ctx, original);
    check(recomp_load8(&rt, c + 4) == 0xc, "Hexen check: queued wheel detent");
    hexen_input_player_hook(&rt, &ctx, original);
    check(recomp_load8(&rt, c + 4) == 0, "Hexen check: wheel repeated");
    update(&rt, &ctx, (RecompMouseFrame){.active = true, .actions = RECOMP_MOUSE_PREVIOUS_WEAPON});
    check(recomp_load8(&rt, c + 4) == 0x14, "Hexen check: previous weapon");
    hexen_input_player_hook(&rt, &ctx, original);
    check(!recomp_load8(&rt, c + 4), "Hexen check: held weapon key repeated");
    recomp_store16(&rt, p + HEXEN_INPUT_READY_ARTIFACT, 7);
    update(&rt, &ctx, (RecompMouseFrame){.active = true, .actions = RECOMP_MOUSE_SECONDARY_ATTACK});
    check(recomp_load8(&rt, c + 6) == 7, "Hexen check: selected artifact");
    hexen_input_player_hook(&rt, &ctx, original);
    check(!recomp_load8(&rt, c + 6), "Hexen check: artifact repeated");
    const uint32_t gates[] = {HEXEN_INPUT_GAMESTATE, HEXEN_INPUT_GAMEACTION, HEXEN_INPUT_DEMO,
        HEXEN_INPUT_PAUSED, HEXEN_INPUT_MENU, p + HEXEN_INPUT_PLAYERSTATE};
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        recomp_store16(&rt, gates[i], 1);
        check(!hexen_input_playable(&rt), "Hexen check: disabled gate");
        recomp_store8(&rt, c, 0x55);
        update(&rt, &ctx, (RecompMouseFrame){.active = true, .move_y = 1, .dx = 100});
        check(recomp_load8(&rt, c) == 0x55, "Hexen check: inactive guest command overwritten");
        recomp_store16(&rt, gates[i], 0);
        hexen_input_player_hook(&rt, &ctx, original);
        check(recomp_load8(&rt, c) == 0x55, "Hexen check: stale motion after gate");
    }
    recomp_store32(&rt, HEXEN_INPUT_PLAYER_COUNT, 2);
    check(!hexen_input_playable(&rt), "Hexen check: multiplayer gate");
    recomp_store32(&rt, HEXEN_INPUT_PLAYER_COUNT, 1);
    update(&rt, &ctx, (RecompMouseFrame){0});
    recomp_store8(&rt, c, 0x33);
    hexen_input_player_hook(&rt, &ctx, original);
    check(recomp_load8(&rt, c) == 0x33, "Hexen check: uncaptured controller changed");
    recomp_runtime_destroy(&rt);
    puts("PASS: Hexen command movement, actions, look, wheel, original calls and capture gates");
    return 0;
}
