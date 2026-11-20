#include "controls.h"
#include "game.h"
#include "game_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled;
static double sensitivity = 0.08;
static RecompMouseBuffer mouse;
static uint32_t down, pressed;
static double tick_dx;

static bool playing(Runtime *rt) {
    return enabled && recomp_load32(rt, DOOM_INPUT_DRAWER) == 1 && !recomp_load32(rt, DOOM_INPUT_PAUSED) &&
           !recomp_load32(rt, DOOM_INPUT_RECORDING) && !recomp_load32(rt, DOOM_INPUT_PLAYBACK) &&
           !recomp_load32(rt, DOOM_INPUT_PLAYER + 4) && recomp_load32(rt, DOOM_INPUT_PLAYER);
}

void doom64_controls_init(RecompRenderer *renderer) {
    const char *mode = getenv("RECOMP_DOOM64_MOUSE");
    enabled = !mode || strcmp(mode, "0");
#ifdef RECOMP_DEBUG
    enabled = enabled && !recomp_game_replaying();
#endif
    if (!enabled)
        return;
    const char *value = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (value && *value) {
        char *end;
        sensitivity = strtod(value, &end);
        recomp_game_require(end != value && !*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    recomp_renderer_separate_mouse_actions(renderer, true);
}

bool doom64_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    recomp_renderer_mouse_mode(renderer, enabled, playing(rt));
    if (!recomp_renderer_input(renderer, pad))
        return false;
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    pressed = frame.active ? pressed | (frame.actions & ~down) : 0;
    down = frame.actions;
    recomp_mouse_submit(&mouse, frame);
    return true;
}

void doom64_player_tick(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    frame.actions |= pressed;
    pressed = 0;
    uint32_t current = recomp_load32(rt, DOOM_INPUT_BUTTONS);
    if (frame.active && playing(rt) && !(current & UINT32_C(0x10000000))) {
        // Doom's menu can reassign every action. Use the game's current
        // BT_DATA bindings so movement, weapons and the automap see real pad bits.
        uint32_t layout = recomp_load32(rt, DOOM_INPUT_BUTTON_LAYOUT);
        const bool actions[] = {frame.move_y > 0,
                                frame.move_y < 0,
                                (frame.actions & RECOMP_MOUSE_ATTACK) != 0,
                                (frame.actions & (RECOMP_MOUSE_USE | RECOMP_MOUSE_SECONDARY_ATTACK)) != 0,
                                (frame.actions & RECOMP_MOUSE_STATUS) != 0,
                                (frame.actions & RECOMP_MOUSE_SPEED) != 0,
                                false, // Strafe modifier; A/D use the dedicated strafes.
                                frame.move_x < 0,
                                frame.move_x > 0,
                                (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON) != 0 || frame.wheel > 0,
                                (frame.actions & RECOMP_MOUSE_NEXT_WEAPON) != 0 || frame.wheel < 0};
        for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
            if (actions[i])
                current |= recomp_load32(rt, layout + 8 + i * 4);
        recomp_store32(rt, DOOM_INPUT_BUTTONS, current);
        if (frame.wheel) {
            uint32_t mask = recomp_load32(rt, layout + (frame.wheel > 0 ? 44 : 48));
            recomp_store32(rt, DOOM_INPUT_OLD_BUTTONS, recomp_load32(rt, DOOM_INPUT_OLD_BUTTONS) & ~mask);
        }
        tick_dx = frame.dx;
    }
    original(rt, ctx);
    tick_dx = 0;
}

void doom64_move_player(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (tick_dx && (uint32_t)ctx->gpr[4] == DOOM_INPUT_PLAYER && !(recomp_load32(rt, DOOM_INPUT_PLAYER + 0x144) & 4)) {
        // One full turn is 2^32. Write the relative yaw once, before the
        // guest applies stick turning and thrust. Automap panning eats turn input.
        uint32_t actor = recomp_load32(rt, DOOM_INPUT_PLAYER);
        double degrees = fmod(tick_dx * sensitivity, 360.0);
        uint32_t delta = (uint32_t)llround(degrees * (4294967296.0 / 360.0));
        recomp_store32(rt, actor + 0x38, recomp_load32(rt, actor + 0x38) - delta);
        tick_dx = 0;
    }
    original(rt, ctx);
}
