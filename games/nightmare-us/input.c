#include "nightmare_input.h"
#include "game.h"
#include "generated.h"
#include "nightmare_input_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled, in_game, injecting;
static uint32_t current_player;
static double sensitivity, turn_remainder;
static RecompMouseBuffer mouse;
static RecompMouseFrame command;

static bool valid_span(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80800000 && !(address & 3) &&
           (address & 0x1fffffff) + size <= rt->rdram_size;
}
static uint32_t player(Runtime *rt) {
    uint32_t p = recomp_load32(rt, NC_INPUT_PLAYER);
    return valid_span(rt, p, 0x400) ? p : 0;
}
bool nightmare_input_enabled(void) { return enabled; }
bool nightmare_input_playable(Runtime *rt) {
    if (!enabled || !in_game) return false;
    uint32_t p = player(rt), state = recomp_load32(rt, NC_INPUT_STATE);
    if (!p || !valid_span(rt, state, 0x24)) return false;
    // Original input/pause checks: menus, demos, fades and disabled controls.
    if ((recomp_load32(rt, state) & 0xc8009870u) ||
        (recomp_load32(rt, state + 4) & 0x03000040u) ||
        !recomp_load16(rt, NC_INPUT_SESSION + 0x22) ||
        !recomp_load16(rt, NC_INPUT_SESSION + 0x24)) return false;
    uint32_t flags = recomp_load32(rt, p + NC_INPUT_PLAYER_FLAGS);
    if (!(flags & 1) || (flags & 0x01780000u)) return false;
    return valid_span(rt, recomp_load32(rt, p + NC_INPUT_TRANSFORM), 0x50);
}
void nightmare_input_init(void) {
    const char *setting = getenv("RECOMP_NIGHTMARE_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_NIGHTMARE_INPUT")) enabled = false;
#endif
    sensitivity = 0.08;
    setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(!*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    in_game = injecting = false;
    current_player = 0;
    turn_remainder = 0;
    recomp_mouse_clear(&mouse);
}
void nightmare_input_submit(RecompMouseFrame frame, RecompPad *pad) {
    recomp_mouse_submit(&mouse, frame);
    if (!frame.active) { turn_remainder = 0; return; }
    // Other gameplay readers must see this game's actions, not the FPS pad map.
    pad->buttons &= 0x1000; // Preserve Start, including a physical controller.
    if (frame.actions & RECOMP_MOUSE_ATTACK) pad->buttons |= 0x4000;
    if (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK) pad->buttons |= 0x8000;
    if (frame.actions & RECOMP_MOUSE_JUMP) pad->buttons |= 0x0002;
    if (frame.actions & RECOMP_MOUSE_DEFEND) pad->buttons |= 0x0004;
    if (frame.actions & RECOMP_MOUSE_USE) pad->buttons |= 0x2000;
    if (frame.actions & RECOMP_MOUSE_STATUS) pad->buttons |= 0x0010;
    if (frame.move_x < 0) pad->buttons |= 0x0008;
    if (frame.move_x > 0) pad->buttons |= 0x0001;
    pad->stick_x = 0;
    pad->stick_y = frame.move_y * ((frame.actions & RECOMP_MOUSE_SPEED) ? 32 : 80);
}
void nightmare_input_game_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool previous = in_game;
    in_game = true;
    original(rt, ctx);
    in_game = previous;
    recomp_mouse_clear(&mouse);
    turn_remainder = 0;
}
void nightmare_input_actions_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t p = (uint32_t)ctx->gpr[4];
    bool active = (uint32_t)ctx->gpr[31] == NC_INPUT_ACTIONS_RETURN &&
                  nightmare_input_playable(rt) && p == player(rt);
    if (!active) {
        recomp_mouse_clear(&mouse);
        turn_remainder = 0;
        original(rt, ctx);
        return;
    }
    command = recomp_mouse_consume(&mouse);
    if (!command.active || !isfinite(command.dx)) { original(rt, ctx); return; }
    current_player = p;
    injecting = true;
    original(rt, ctx);
    injecting = false;
    current_player = 0;
    if (!nightmare_input_playable(rt) || player(rt) != p) {
        recomp_mouse_clear(&mouse);
        turn_remainder = 0;
        return;
    }
    // Native heading is 16 units per 1/4096 turn. Left increases heading.
    // Apply once before animation/movement; leave camera follow and combat intact.
    uint32_t pose = recomp_load32(rt, p + NC_INPUT_TRANSFORM);
    double delta = fmod(-command.dx * sensitivity * (65536.0 / 360.0) + turn_remainder, 65536.0);
    int32_t whole = (int32_t)delta;
    turn_remainder = delta - whole;
    uint32_t heading = recomp_load32(rt, pose + NC_INPUT_HEADING);
    recomp_store32(rt, pose + NC_INPUT_HEADING, (heading + (uint32_t)whole) & 0xffffu);
}
void nightmare_input_read_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool active = injecting && (uint32_t)ctx->gpr[31] == NC_INPUT_READ_RETURN;
    original(rt, ctx);
    if (!active || !nightmare_input_playable(rt) || current_player != player(rt)) return;
    // The game-facing pad is active-high PSX-style bits, not N64 button bits.
    uint32_t buttons = (uint32_t)ctx->gpr[2] & 8; // Start remains native.
    if (command.move_y > 0) buttons |= 0x10;
    if (command.move_y < 0) buttons |= 0x40;
    if (command.move_x < 0) buttons |= 0x400;
    if (command.move_x > 0) buttons |= 0x800;
    if (command.actions & RECOMP_MOUSE_ATTACK) buttons |= 0x8000;
    if (command.actions & RECOMP_MOUSE_SECONDARY_ATTACK) buttons |= 0x4000;
    if (command.actions & RECOMP_MOUSE_JUMP) buttons |= 0x1000;
    if (command.actions & RECOMP_MOUSE_DEFEND) buttons |= 0x2000;
    if ((command.actions & RECOMP_MOUSE_SPEED) && command.move_y) buttons |= 0x2000;
    if (command.actions & RECOMP_MOUSE_USE) buttons |= 1;
    if (command.actions & RECOMP_MOUSE_STATUS) buttons |= 0x100;
    bool previous = (command.actions & RECOMP_MOUSE_PREVIOUS_WEAPON) != 0;
    bool next = (command.actions & RECOMP_MOUSE_NEXT_WEAPON) != 0;
    if (command.wheel || previous || next) {
        // Native inventory selection is R + left/right. Re-arm wheel edges.
        buttons |= 0x100 | ((command.wheel ? command.wheel > 0 : previous) ? 0x80 : 0x20);
        if (command.wheel) recomp_store32(rt, current_player + NC_INPUT_PREVIOUS,
                            recomp_load32(rt, current_player + NC_INPUT_PREVIOUS) & ~0xa0u);
    }
    recomp_store8(rt, NC_INPUT_ANALOG_X, 128);
    int magnitude = (command.actions & RECOMP_MOUSE_SPEED) ? 48 : 127;
    recomp_store8(rt, NC_INPUT_ANALOG_Y, (uint8_t)(128 - command.move_y * magnitude));
    ctx->gpr[2] = buttons;
}
