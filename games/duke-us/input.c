#include "duke_input.h"
#include "duke_input_config.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled, process_ready, wheel_gap;
static double sensitivity, vertical, yaw_remainder, pitch_remainder;
static RecompMouseBuffer mouse;
static void (*poll_live_input)(Runtime *, void *);
static void clear(void) {
    recomp_mouse_clear(&mouse);
    process_ready = wheel_gap = false;
    yaw_remainder = pitch_remainder = 0;
}
bool duke_input_playable(Runtime *rt) {
    return enabled && recomp_load8(rt, DUKE_INPUT_GAME_STATE) == 2 && !recomp_load8(rt, DUKE_INPUT_INACTIVE) &&
           recomp_load16(rt, DUKE_INPUT_PLAYERS) == 1 && !recomp_load16(rt, DUKE_INPUT_USER + DUKE_INPUT_PAUSED) &&
           !recomp_load8(rt, DUKE_INPUT_USER + DUKE_INPUT_OVERHEAD) &&
           (int16_t)recomp_load16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_HEALTH) > 0 &&
           !recomp_load16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_FIST) &&
           (int16_t)recomp_load16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_NEW_OWNER) < 0;
}
void duke_input_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    const char *setting = getenv("RECOMP_DUKE_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_DUKE_INPUT"))
        enabled = false;
#endif
    sensitivity = 0.08;
    setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(end != setting && !*end && isfinite(sensitivity) && sensitivity >= 0.001 &&
                                sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    setting = getenv("RECOMP_MOUSE_INVERT_Y");
    vertical = setting && !strcmp(setting, "1") ? -1 : 1;
    clear();
}
void duke_input_submit(RecompMouseFrame frame) {
    if (!frame.active)
        clear();
    recomp_mouse_submit(&mouse, frame);
}
bool duke_input_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    recomp_renderer_mouse_mode(renderer, enabled, duke_input_playable(rt));
    if (!recomp_renderer_input(renderer, pad)) {
        duke_input_submit((RecompMouseFrame){0});
        return false;
    }
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    duke_input_submit(frame);
    if (frame.active) {
        pad->buttons &= 0x1000; // Keep Start; the virtual map below owns gameplay actions.
        pad->stick_x = pad->stick_y = 0;
    }
    return true;
}
void duke_input_command_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    process_ready = false;
    if ((uint32_t)ctx->gpr[4] != 0 || (uint32_t)ctx->gpr[31] != DUKE_INPUT_COMMAND_RETURN || !duke_input_playable(rt)) {
        clear();
        original(rt, ctx);
        return;
    }
    if (poll_live_input)
        recomp_on_host(rt, poll_live_input, NULL);
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active || !duke_input_playable(rt) || !isfinite(frame.dx) || !isfinite(frame.dy)) {
        clear();
        original(rt, ctx);
        return;
    }
    double yaw = fmod(recomp_load16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW) + yaw_remainder +
                          frame.dx * sensitivity * (2048.0 / 360.0),
                      2048.0);
    if (yaw < 0)
        yaw += 2048;
    uint16_t angle = (uint16_t)yaw;
    yaw_remainder = yaw - angle;
    const double radians = 3.14159265358979323846 / 180.0;
    double horizon = (int32_t)recomp_load32(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_PITCH) + pitch_remainder;
    double pitch = atan((horizon - 100) / 128) / radians - frame.dy * sensitivity * vertical;
    pitch = fmax(-45, fmin(45, pitch));
    horizon = 100 + 128 * tan(pitch * radians);
    int32_t rounded = (int32_t)lround(horizon);
    pitch_remainder = horizon - rounded;
    recomp_store16(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_YAW, angle);
    recomp_store32(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_PITCH, (uint32_t)rounded);

    // Let the ROM construct its own world-space movement, friction and action
    // bits through a temporary simple control map. No settings/save changes.
    uint32_t saved_map[16];
    for (unsigned i = 0; i < 16; i++) {
        saved_map[i] = recomp_load32(rt, DUKE_INPUT_CONTROL_MAP + i * 4);
        recomp_store32(rt, DUKE_INPUT_CONTROL_MAP + i * 4, (i == 3 || i == 4 || i == 15) ? 0 : 1u << i);
    }
    uint16_t saved_controls[] = {recomp_load16(rt, DUKE_INPUT_CONTROL_BUTTONS), recomp_load16(rt, DUKE_INPUT_CONTROL_X),
                                 recomp_load16(rt, DUKE_INPUT_CONTROL_Y)};
    uint16_t saved_work[4];
    for (unsigned i = 0; i < 4; i++)
        saved_work[i] = recomp_load16(rt, DUKE_INPUT_WORK_CONTROLS + i * 2);
    uint8_t saved_aim = recomp_load8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE);
    uint16_t buttons = 0;
    if (frame.actions & RECOMP_MOUSE_JUMP)
        buttons |= 1u << 0;
    if (frame.actions & (RECOMP_MOUSE_DEFEND | RECOMP_MOUSE_SECONDARY_ATTACK))
        buttons |= 1u << 1;
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        buttons |= 1u << 2;
    if (frame.actions & RECOMP_MOUSE_USE)
        buttons |= 1u << 9;
    if (frame.actions & RECOMP_MOUSE_STATUS)
        buttons |= 1u << 10;
    if (frame.move_y > 0)
        buttons |= 1u << 11;
    if (frame.move_y < 0)
        buttons |= 1u << 12;
    if (frame.move_x < 0)
        buttons |= 1u << 13;
    if (frame.move_x > 0)
        buttons |= 1u << 14;
    if (wheel_gap) {
        mouse.pending.wheel = recomp_mouse_wheel_sum(mouse.pending.wheel, frame.wheel);
        frame.wheel = 0;
        wheel_gap = false;
    } else if (frame.wheel)
        wheel_gap = true;
    if (frame.wheel > 0 || (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON))
        buttons |= 1u << 5;
    else if (frame.wheel < 0 || (frame.actions & RECOMP_MOUSE_NEXT_WEAPON))
        buttons |= 1u << 6;
    recomp_store16(rt, DUKE_INPUT_CONTROL_BUTTONS, buttons);
    recomp_store16(rt, DUKE_INPUT_CONTROL_X, 0);
    recomp_store16(rt, DUKE_INPUT_CONTROL_Y, 0);
    recomp_store8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE, 0);
    original(rt, ctx);
    for (unsigned i = 0; i < 16; i++)
        recomp_store32(rt, DUKE_INPUT_CONTROL_MAP + i * 4, saved_map[i]);
    recomp_store16(rt, DUKE_INPUT_CONTROL_BUTTONS, saved_controls[0]);
    recomp_store16(rt, DUKE_INPUT_CONTROL_X, saved_controls[1]);
    recomp_store16(rt, DUKE_INPUT_CONTROL_Y, saved_controls[2]);
    for (unsigned i = 0; i < 4; i++)
        recomp_store16(rt, DUKE_INPUT_WORK_CONTROLS + i * 2, saved_work[i]);
    recomp_store8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE, saved_aim);
    process_ready = true;
}
void duke_input_process_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (!process_ready || (uint32_t)ctx->gpr[4] != 0 || (uint32_t)ctx->gpr[31] != DUKE_INPUT_PROCESS_RETURN ||
        !duke_input_playable(rt)) {
        clear();
        original(rt, ctx);
        return;
    }
    process_ready = false;
    uint8_t aim = recomp_load8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE);
    // Native free-look suppresses slope auto-look and near-center snapping.
    // Hard landing, recoil, collision, weapons and all other effects still run.
    recomp_store8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE, 1);
    recomp_store8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_CENTERING, 0);
    recomp_store8(rt, DUKE_INPUT_SYNC + 1, 0);
    original(rt, ctx);
    recomp_store8(rt, DUKE_INPUT_PLAYER + DUKE_INPUT_AIM_MODE, aim);
    if (!duke_input_playable(rt))
        clear();
}
