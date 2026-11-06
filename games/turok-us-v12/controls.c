#include "controls.h"
#include "turok_controls_config.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define radians (3.14159265358979323846 / 180.0)
static RecompFov fov;
static RecompMouseBuffer mouse;
static RecompMouseFrame command;
static void (*poll_live_input)(Runtime *, void *);
static bool enabled, injecting, jump_pressed, gameplay_update, command_ready;
static uint32_t previous_actions, pressed, released;
static double sensitivity, invert_y;
static float desired_pitch;

static float from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint32_t to_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static float read_float(Runtime *rt, uint32_t address) {
    return from_bits(recomp_load32(rt, address));
}
static void write_float(Runtime *rt, uint32_t address, float value) {
    recomp_store32(rt, address, to_bits(value));
}
static bool valid(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80400000 && !(address & 3) && size <= rt->rdram_size &&
           (address & 0x1fffffff) <= rt->rdram_size - size;
}
static uint32_t live_player(Runtime *rt) {
    if (recomp_load32(rt, T_CTL_ENGINE + T_CTL_MODE) != 4 || recomp_load32(rt, T_CTL_ENGINE + T_CTL_PAUSE) ||
        recomp_load32(rt, T_CTL_ENGINE + T_CTL_WARP) || recomp_load32(rt, T_CTL_ENGINE + T_CTL_DEATH) ||
        recomp_load32(rt, T_CTL_ENGINE + T_CTL_TRAINING) || recomp_load16(rt, T_CTL_CAMERA + T_CTL_CAMERA_MODE) != 7 ||
        recomp_load32(rt, T_CTL_CAMERA + T_CTL_CINEMA) || recomp_load32(rt, T_CTL_ATTRACT) ||
        recomp_load32(rt, T_CTL_ATTRACT_RECORD))
        return 0;
    uint32_t player = recomp_load32(rt, T_CTL_ENGINE + T_CTL_PLAYER);
    return valid(rt, player, T_CTL_HEALTH + 4) && (int16_t)recomp_load16(rt, player + T_CTL_HEALTH) > 0 ? player : 0;
}
void turok_controls_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    recomp_mouse_clear(&mouse);
    injecting = false;
    previous_actions = 0;
    const char *setting = getenv("RECOMP_TUROK_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (recomp_game_replaying())
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
    invert_y = setting && !strcmp(setting, "1") ? 1 : -1;
    recomp_fov_init(&fov, from_bits(T_CTL_VERTICAL_BITS), (double)T_CTL_SCREEN_WIDTH / T_CTL_SCREEN_HEIGHT);
}
void turok_fov_adjust(int steps) {
    recomp_fov_adjust(&fov, steps);
}
bool turok_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    recomp_renderer_mouse_mode(renderer, enabled, enabled && live_player(rt));
    if (!recomp_renderer_input(renderer, pad)) {
        recomp_mouse_clear(&mouse);
        command.active = false;
        previous_actions = 0;
        return false;
    }
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    recomp_mouse_submit(&mouse, frame);
    if (frame.active) {
        // Start remains native; gameplay action queries below own captured input.
        pad->buttons &= 0x1000;
        pad->stick_x = pad->stick_y = 0;
    } else {
        command.active = false;
        previous_actions = 0;
        // Preserve the existing live controller remap. Replays replace this pad
        // through recomp_game_input and retain original N64 semantics.
        uint16_t look = pad->buttons & 0x000c;
        int y = pad->stick_y;
        pad->buttons &= ~0x000c;
        if (y > 20)
            pad->buttons |= 0x0008;
        if (y < -20)
            pad->buttons |= 0x0004;
        pad->stick_y = ((look & 0x0008) != 0) * 80 - ((look & 0x0004) != 0) * 80;
    }
    return true;
}
void turok_update_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    gameplay_update = (uint32_t)ctx->gpr[4] == T_CTL_ENGINE;
    command_ready = false;
    original(rt, ctx);
    gameplay_update = command_ready = false;
}
void turok_controller_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool scoped = enabled && gameplay_update && live_player(rt) && (uint32_t)ctx->gpr[31] == T_CTL_CONTROLLER_RETURN;
    if (scoped && poll_live_input)
        recomp_on_host(rt, poll_live_input, NULL);
    command = recomp_mouse_consume(&mouse);
    command_ready = scoped && command.active && live_player(rt) && isfinite(command.dx) && isfinite(command.dy);
    if (!command_ready) {
        recomp_mouse_clear(&mouse);
        previous_actions = 0;
        original(rt, ctx);
        return;
    }
    pressed = command.actions & ~previous_actions;
    released = previous_actions & ~command.actions;
    const uint32_t jump = RECOMP_MOUSE_JUMP | RECOMP_MOUSE_SECONDARY_ATTACK;
    jump_pressed = !(previous_actions & jump) && (command.actions & jump);
    previous_actions = command.actions;
    original(rt, ctx);
}
void turok_movement_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t player = live_player(rt);
    injecting = command_ready && player && (uint32_t)ctx->gpr[31] == T_CTL_MOVEMENT_RETURN &&
                (uint32_t)ctx->gpr[4] == T_CTL_MOVEMENT && (uint32_t)ctx->gpr[5] == T_CTL_ENGINE &&
                (uint32_t)ctx->gpr[6] == T_CTL_CONTROL;
    if (!injecting) {
        original(rt, ctx);
        return;
    }
    float pitch = read_float(rt, T_CTL_MOVEMENT + T_CTL_PITCH);
    float yaw = read_float(rt, player + T_CTL_YAW);
    // Climbing owns the native head/ledge camera. Keep its movement controls.
    if (recomp_load8(rt, T_CTL_MOVEMENT + T_CTL_MOVE_MODE) != 5 && isfinite(pitch) && isfinite(yaw)) {
        desired_pitch =
            (float)fmax(-78 * radians, fmin(90 * radians, pitch + command.dy * sensitivity * invert_y * radians));
        double next_yaw = remainder(yaw + command.dx * sensitivity * radians, 2 * 3.14159265358979323846);
        write_float(rt, player + T_CTL_YAW, (float)next_yaw);
        write_float(rt, T_CTL_MOVEMENT + T_CTL_TURN_SPEED, 0);
        write_float(rt, T_CTL_MOVEMENT + T_CTL_ACTUAL_TURN_SPEED, 0);
    } else {
        desired_pitch = NAN;
    }
    original(rt, ctx);
    injecting = false;
}
void turok_pitch_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (injecting && isfinite(desired_pitch) && live_player(rt) && (uint32_t)ctx->gpr[31] == T_CTL_PITCH_RETURN &&
        (uint32_t)ctx->gpr[4] == T_CTL_MOVEMENT + T_CTL_PITCH) {
        // Replace the spring-to-centre result before native camera, movement and
        // weapon code consume it. Recoil and head bob are added later by the ROM.
        write_float(rt, T_CTL_MOVEMENT + T_CTL_PITCH, desired_pitch);
        write_float(rt, T_CTL_MOVEMENT + T_CTL_TARGET_PITCH, desired_pitch);
    }
    original(rt, ctx);
}
void turok_action_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t caller = (uint32_t)ctx->gpr[31];
    uint32_t type = (uint32_t)ctx->gpr[7];
    bool scoped = command_ready && command.active && (uint32_t)ctx->gpr[4] == T_CTL_CONTROL && live_player(rt);
    original(rt, ctx);
    if (!scoped)
        return;
    bool down;
    switch (caller) {
        case T_CTL_FIRE_RETURN:
            down = ((type == 3 ? pressed : type == 2 ? released : command.actions) & RECOMP_MOUSE_ATTACK) != 0;
            break;
        case T_CTL_JUMP_RETURN:
        case T_CTL_SWIM_UP_RETURN:
            down = (command.actions & (RECOMP_MOUSE_JUMP | RECOMP_MOUSE_SECONDARY_ATTACK)) != 0;
            break;
        case T_CTL_START_JUMP_RETURN:
            down = jump_pressed;
            break;
        case T_CTL_WEAPON_PREVIOUS_RETURN:
            down = command.wheel > 0 || (pressed & RECOMP_MOUSE_PREVIOUS_WEAPON);
            break;
        case T_CTL_WEAPON_NEXT_RETURN:
            down = command.wheel < 0 || (pressed & RECOMP_MOUSE_NEXT_WEAPON);
            break;
        case T_CTL_FORWARD_RETURN:
        case T_CTL_SWIM_FORWARD_RETURN:
        case T_CTL_CLIMB_UP_RETURN:
        case T_CTL_MAP_UP_RETURN:
            down = command.move_y > 0;
            break;
        case T_CTL_BACKWARD_RETURN:
        case T_CTL_SWIM_BACKWARD_RETURN:
        case T_CTL_MAP_DOWN_RETURN:
            down = command.move_y < 0;
            break;
        case T_CTL_SIDE_LEFT_RETURN:
        case T_CTL_SWIM_LEFT_RETURN:
        case T_CTL_CLIMB_LEFT_RETURN:
        case T_CTL_MAP_LEFT_RETURN:
            down = command.move_x < 0;
            break;
        case T_CTL_SIDE_RIGHT_RETURN:
        case T_CTL_SWIM_RIGHT_RETURN:
        case T_CTL_CLIMB_RIGHT_RETURN:
        case T_CTL_MAP_RIGHT_RETURN:
            down = command.move_x > 0;
            break;
        case T_CTL_MAP_RETURN:
            down = (command.actions & RECOMP_MOUSE_STATUS) != 0;
            break;
        case T_CTL_LOOK_UP_RETURN:
        case T_CTL_LOOK_DOWN_RETURN:
        case T_CTL_LOOK_LEFT_RETURN:
        case T_CTL_LOOK_RIGHT_RETURN:
        case T_CTL_TURN_LEFT_RETURN:
        case T_CTL_TURN_RIGHT_RETURN:
        case T_CTL_ROLL_LEFT_RETURN:
        case T_CTL_ROLL_RIGHT_RETURN:
        case T_CTL_RUN_FORWARD_RETURN:
        case T_CTL_RUN_BACKWARD_RETURN:
        case T_CTL_RUN_TOGGLE_RETURN:
            down = false;
            break;
        default:
            return;
    }
    // The original action ABI uses -1 for held digital actions, zero for idle.
    ctx->fpr[0] = to_bits(down ? -1.0f : 0.0f);
}
void turok_projection_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t sp = (uint32_t)ctx->gpr[29];
    float native = from_bits((uint32_t)ctx->gpr[6]);
    float aspect = from_bits((uint32_t)ctx->gpr[7]);
    if (recomp_fov_enabled(&fov) && live_player(rt) && (uint32_t)ctx->gpr[31] == T_CTL_PROJECTION_RETURN &&
        (uint32_t)ctx->gpr[5] == T_CTL_CAMERA + T_CTL_PERSP_NORM &&
        (uint32_t)ctx->gpr[4] == sp + T_CTL_PROJECTION_STACK && valid(rt, sp, T_CTL_CAMERA_STACK_SIZE) &&
        isfinite(native) && native > 0 && native < 150 && isfinite(aspect) && aspect >= 1 && aspect <= 2) {
        double scale = tan(fov.horizontal * radians / 2) / tan(fov.original * radians / 2);
        double vertical = 2 * atan(tan(native * radians / 2) * scale) / radians;
        double horizontal = 2 * atan(tan(vertical * radians / 2) * aspect) / radians;
        // The CPU uses fov*aspect to build a conservative five-corner volume.
        // Change its saved local too, retaining native overscan and bounding the
        // angle below 180 even during underwater aspect/zoom effects.
        double cpu_horizontal = fmin(178, fmax(native * aspect, horizontal + 2));
        mips_set(ctx, 6, to_bits((float)vertical));
        write_float(rt, T_CTL_CAMERA + T_CTL_CAMERA_FOV, (float)vertical);
        write_float(rt, sp + T_CTL_CPU_FOV_STACK, (float)(cpu_horizontal / aspect));
    }
    original(rt, ctx);
}
