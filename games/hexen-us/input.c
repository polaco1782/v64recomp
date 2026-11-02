#include "hexen_input.h"
#include "game.h"
#include "hexen_input_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled;
static double sensitivity, vertical, yaw_remainder, pitch_remainder;
static uint32_t previous_actions, current_actor;
static RecompMouseBuffer mouse;
static void (*poll_live_input)(Runtime *, void *);

static bool valid_actor(Runtime *rt, uint32_t address) {
    return address >= 0x80000000 && address < 0x80400000 && !(address & 3) &&
           (address & 0x1fffffff) + 0xa0 <= rt->rdram_size;
}
static void clear(void) {
    recomp_mouse_clear(&mouse);
    previous_actions = current_actor = 0;
    yaw_remainder = pitch_remainder = 0;
}
bool hexen_input_enabled(void) {
    return enabled;
}
bool hexen_input_playable(Runtime *rt) {
    if (!enabled || recomp_load16(rt, HEXEN_INPUT_GAMESTATE) || recomp_load16(rt, HEXEN_INPUT_GAMEACTION) ||
        recomp_load16(rt, HEXEN_INPUT_DEMO) || recomp_load16(rt, HEXEN_INPUT_PAUSED) ||
        recomp_load16(rt, HEXEN_INPUT_MENU) || recomp_load32(rt, HEXEN_INPUT_PLAYER_COUNT) != 1 ||
        !recomp_load16(rt, HEXEN_INPUT_PLAYERINGAME) ||
        recomp_load16(rt, HEXEN_INPUT_PLAYERS + HEXEN_INPUT_PLAYERSTATE))
        return false;
    return valid_actor(rt, recomp_load32(rt, HEXEN_INPUT_PLAYERS));
}
void hexen_input_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    const char *setting = getenv("RECOMP_HEXEN_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_HEXEN_INPUT"))
        enabled = false;
#endif
    sensitivity = 0.10;
    setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(!*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    setting = getenv("RECOMP_MOUSE_INVERT_Y");
    vertical = setting && !strcmp(setting, "1") ? -1 : 1;
    clear();
}
void hexen_input_submit(RecompMouseFrame frame, RecompPad *pad) {
    if (!frame.active)
        clear();
    recomp_mouse_submit(&mouse, frame);
    if (!frame.active)
        return;
    // Captured input owns gameplay actions. Keep Start and the native automap
    // event; the generic FPS A/B/Z/R bits have different meanings in Hexen.
    pad->buttons &= 0x1000;
    if (frame.actions & RECOMP_MOUSE_STATUS)
        pad->buttons |= 0x0008;
    pad->stick_x = pad->stick_y = 0;
}
void hexen_input_player_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t player = (uint32_t)ctx->gpr[4];
    if (player != HEXEN_INPUT_PLAYERS || (uint32_t)ctx->gpr[31] != HEXEN_INPUT_PLAYER_RETURN ||
        !hexen_input_playable(rt)) {
        clear();
        original(rt, ctx);
        return;
    }
    // SDL belongs to the host thread. Poll at each simulation update, including
    // updates serviced by frame pacing rather than a presentation boundary.
    if (poll_live_input)
        recomp_on_host(rt, poll_live_input, NULL);
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active || !hexen_input_playable(rt) || !isfinite(frame.dx) || !isfinite(frame.dy)) {
        clear();
        original(rt, ctx);
        return;
    }
    uint32_t actor = recomp_load32(rt, player);
    if (actor != current_actor) {
        yaw_remainder = pitch_remainder = 0;
        previous_actions = 0;
        current_actor = actor;
    }
    uint32_t command = player + HEXEN_INPUT_COMMAND;
    unsigned cls = recomp_load16(rt, player + HEXEN_INPUT_CLASS);
    if (cls >= 4) {
        clear();
        original(rt, ctx);
        return;
    }
    unsigned speed = (frame.actions & RECOMP_MOUSE_SPEED) != 0;
    int forward = (int32_t)recomp_load32(rt, HEXEN_INPUT_FORWARD_TABLE + cls * 8 + speed * 4);
    int side = (int32_t)recomp_load32(rt, HEXEN_INPUT_SIDE_TABLE + cls * 8 + speed * 4);
    if (recomp_load16(rt, player + HEXEN_INPUT_SPEED_POWER) && !recomp_load16(rt, player + HEXEN_INPUT_MORPH)) {
        forward = forward * 3 / 2;
        side = side * 3 / 2;
    }
    recomp_store8(rt, command + HEXEN_INPUT_FORWARD, (uint8_t)(frame.move_y * forward));
    recomp_store8(rt, command + HEXEN_INPUT_SIDE, (uint8_t)(frame.move_x * side));
    double yaw = fmod(-frame.dx * sensitivity * (65536.0 / 360.0) + yaw_remainder, 65536.0);
    int32_t turn = (int32_t)yaw;
    yaw_remainder = yaw - turn;
    recomp_store16(rt, command + HEXEN_INPUT_TURN, (uint16_t)turn);
    double pitch =
        (int16_t)recomp_load16(rt, player + HEXEN_INPUT_LOOKDIR) + pitch_remainder - frame.dy * sensitivity * vertical;
    pitch = fmax(-110, fmin(90, pitch));
    int32_t look = (int32_t)pitch;
    pitch_remainder = pitch - look;
    recomp_store16(rt, player + HEXEN_INPUT_LOOKDIR, (uint16_t)look);
    recomp_store16(rt, player + HEXEN_INPUT_CENTERING, 0);
    recomp_store8(rt, command + HEXEN_INPUT_LOOKFLY, 0);
    uint32_t pressed = frame.actions & ~previous_actions;
    previous_actions = frame.actions;
    uint8_t buttons = 0;
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        buttons |= 1;
    if (frame.actions & RECOMP_MOUSE_USE)
        buttons |= 2;
    if (frame.wheel < 0 || (pressed & RECOMP_MOUSE_NEXT_WEAPON))
        buttons |= 0x0c;
    else if (frame.wheel > 0 || (pressed & RECOMP_MOUSE_PREVIOUS_WEAPON))
        buttons |= 0x14;
    recomp_store8(rt, command + HEXEN_INPUT_BUTTONS, buttons);
    uint8_t artifact = (frame.actions & RECOMP_MOUSE_JUMP) ? 0x80 : 0;
    if (pressed & RECOMP_MOUSE_SECONDARY_ATTACK)
        artifact |= recomp_load16(rt, player + HEXEN_INPUT_READY_ARTIFACT) & 0x3f;
    recomp_store8(rt, command + HEXEN_INPUT_ARTIFACT, artifact);
    original(rt, ctx);
    if (!hexen_input_playable(rt))
        clear();
}
