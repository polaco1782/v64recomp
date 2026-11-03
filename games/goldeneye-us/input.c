#include "goldeneye_input.h"
#include "game.h"
#include "generated.h"
#include "goldeneye_input_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

static FILE *trace;
static uint64_t moves, polls, samples, rows;
static bool enabled, in_move;
static bool gamepad_enabled, in_dual_input;
static RecompCameraStick aim_stick;
static int8_t move_x, move_y;
static uint16_t aim_buttons;
static double sensitivity = 0.08;
static double vertical = 1;
static RecompMouseBuffer mouse;
static RecompMouseBuffer menu_mouse;
#ifdef RECOMP_DEBUG
static FILE *replay;
static uint64_t next_move, previous_move, replay_updates;
static RecompMouseFrame replay_frame;
static void read_replay(void) {
    unsigned long long step;
    int x, y, active;
    double dx, dy;
    int n = fscanf(replay, "%llu %lf %lf %d %d %d", &step, &dx, &dy, &x, &y, &active);
    if (n == EOF) {
        recomp_game_require(!ferror(replay), "GoldenEye mouse replay read failed", 0);
        next_move = 0;
        return;
    }
    recomp_game_require(n == 6 && step > previous_move && isfinite(dx) && isfinite(dy) && fabs(dx) <= 100000 &&
                            fabs(dy) <= 100000 && x >= -1 && x <= 1 && y >= -1 && y <= 1 &&
                            (active == 0 || active == 1),
                        "invalid GoldenEye mouse replay", 0);
    next_move = previous_move = step;
    replay_frame = (RecompMouseFrame){dx, dy, (int8_t)x, (int8_t)y, active != 0, 0, 0};
}
#endif
static void write_float(Runtime *rt, uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    recomp_store32(rt, address, bits);
}
static float read_float(Runtime *rt, uint32_t address) {
    uint32_t bits = recomp_load32(rt, address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint32_t player(Runtime *rt) {
    uint32_t p = recomp_load32(rt, GE_INPUT_CURRENT_PLAYER);
    return p >= 0x80000000 && p < 0x80400000 && (p & 3) == 0 && (p & 0x1fffffff) + 0x2a5c <= rt->rdram_size ? p : 0;
}
bool goldeneye_input_enabled(void) {
    return enabled;
}
bool goldeneye_input_playable(Runtime *rt) {
    uint32_t p = player(rt);
    if (!p || recomp_load32(rt, GE_INPUT_STAGE_ADDRESS) == GE_INPUT_TITLE_STAGE ||
        recomp_load32(rt, GE_INPUT_PLAYERS) != p || recomp_load32(rt, GE_INPUT_PLAYERS + 4) ||
        recomp_load32(rt, GE_INPUT_PLAYERS + 8) || recomp_load32(rt, GE_INPUT_PLAYERS + 12) ||
        recomp_load32(rt, GE_INPUT_CONTROLS_LOCKED) || recomp_load32(rt, GE_INPUT_TANK) ||
        recomp_load32(rt, p + GE_INPUT_DEAD) || recomp_load32(rt, p + GE_INPUT_WATCH_STATE))
        return false;
    uint32_t camera = recomp_load32(rt, GE_INPUT_CAMERA);
    return camera == 0 || (camera == 4 && recomp_load32(rt, GE_INPUT_TIMER_ACTIVE));
}
bool goldeneye_input_menu(Runtime *rt) {
    return enabled && recomp_load32(rt, GE_INPUT_STAGE_ADDRESS) == GE_INPUT_TITLE_STAGE;
}
bool goldeneye_input_gamepad_mode(Runtime *rt) {
    return gamepad_enabled && goldeneye_input_playable(rt);
}
void goldeneye_input_submit(Runtime *rt, RecompMouseFrame frame, RecompCameraStick aim, RecompPad *pad) {
    aim_stick = goldeneye_input_gamepad_mode(rt) ? aim : (RecompCameraStick){0};
#ifdef RECOMP_DEBUG
    if (replay)
        return;
#endif
    if (goldeneye_input_menu(rt)) {
        recomp_mouse_clear(&mouse);
        recomp_mouse_submit(&menu_mouse, frame);
        if (frame.active) {
            if (frame.actions & RECOMP_MOUSE_ATTACK)
                pad->buttons |= 0x8000;
            if (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK)
                pad->buttons |= 0x4000;
        }
        return;
    }
    recomp_mouse_clear(&menu_mouse);
    recomp_mouse_submit(&mouse, frame);
}
void goldeneye_input_menu_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    RecompMouseFrame frame = recomp_mouse_consume(&menu_mouse);
    if (goldeneye_input_menu(rt) && frame.active && isfinite(frame.dx) && isfinite(frame.dy)) {
        float x = read_float(rt, GE_INPUT_MENU_X), y = read_float(rt, GE_INPUT_MENU_Y);
        if (isfinite(x) && isfinite(y)) {
            // Menu coordinates are pixels. Consume motion once, without the
            // gameplay pitch inversion or frame-time scaling. The original
            // still adds keyboard/gamepad movement and clamps to its viewport.
            write_float(rt, GE_INPUT_MENU_X, (float)(x + frame.dx * 0.5));
            write_float(rt, GE_INPUT_MENU_Y, (float)(y + frame.dy * 0.5));
        }
    }
    original(rt, ctx);
}
static void observe(Runtime *rt, MipsContext *ctx, const char *event) {
    if (!trace || rows++ >= 30000)
        return;
    uint32_t p = player(rt);
    fprintf(trace,
            "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%08x,%08x,%u,%u,%u,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%u,%.6f,%.6f,%u\n",
            event, recomp_count64(rt), moves, polls, samples, (uint32_t)ctx->gpr[31], p,
            p ? recomp_load32(rt, p + GE_INPUT_DEAD) : 0, p ? recomp_load32(rt, p + GE_INPUT_WATCH_STATE) : 0,
            p ? recomp_load32(rt, p + GE_INPUT_AIM_MODE) : 0, p ? read_float(rt, p + GE_INPUT_YAW) : 0,
            p ? read_float(rt, p + GE_INPUT_PITCH) : 0, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
            (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], recomp_load32(rt, GE_INPUT_CAMERA),
            recomp_load32(rt, GE_INPUT_CONTROLS_LOCKED), recomp_load32(rt, GE_INPUT_TANK),
            p ? read_float(rt, p + GE_INPUT_SIDE_SPEED) : 0, p ? read_float(rt, p + GE_INPUT_FORWARD_SPEED) : 0,
            p ? recomp_load32(rt, p + GE_INPUT_CONTROL_STYLE) : 0);
}
void goldeneye_input_init(Runtime *rt) {
    (void)rt;
    moves = polls = samples = rows = 0;
    sensitivity = 0.08;
    in_move = false;
    enabled = !getenv("RECOMP_GOLDENEYE_MOUSE") || strcmp(getenv("RECOMP_GOLDENEYE_MOUSE"), "0");
    gamepad_enabled = !getenv("RECOMP_GOLDENEYE_GAMEPAD") || strcmp(getenv("RECOMP_GOLDENEYE_GAMEPAD"), "0");
    in_dual_input = false;
    aim_stick = (RecompCameraStick){0};
    const char *setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(!*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    vertical = getenv("RECOMP_MOUSE_INVERT_Y") && !strcmp(getenv("RECOMP_MOUSE_INVERT_Y"), "1") ? -1 : 1;
    recomp_mouse_clear(&mouse);
    recomp_mouse_clear(&menu_mouse);
#ifdef RECOMP_DEBUG
    // Legacy controller replays must not inherit physical mouse state.
    if (recomp_game_replaying())
        enabled = gamepad_enabled = false;
    const char *replay_path = getenv("RECOMP_GOLDENEYE_MOUSE_REPLAY");
    if (replay_path && *replay_path) {
        replay = fopen(replay_path, "r");
        recomp_game_require(replay != NULL, "cannot open GoldenEye mouse replay", 0);
        previous_move = replay_updates = 0;
        enabled = true;
        gamepad_enabled = false;
        read_replay();
    }
    const char *path = getenv("RECOMP_GOLDENEYE_INPUT_TRACE");
    if (path && *path) {
        trace = fopen(path, "w");
        recomp_game_require(trace != NULL, "cannot open GoldenEye input trace", 0);
        fputs("event,ticks,moves,polls,samples,return_pc,player,dead,watch,aim,yaw,pitch,a0,a1,a2,a3,camera,locked,"
              "tank,side_speed,forward_speed,style\n",
              trace);
    }
#endif
}
void goldeneye_input_destroy(void) {
    if (trace)
        fclose(trace);
    trace = NULL;
#ifdef RECOMP_DEBUG
    if (replay)
        fclose(replay);
    replay = NULL;
#endif
}
void goldeneye_input_poll_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    polls++;
    original(rt, ctx);
}
void goldeneye_input_samples_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    samples++;
    original(rt, ctx);
}
void goldeneye_input_move_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    moves++;
    observe(rt, ctx, "move");
    bool previous = in_move;
    in_move = true;
    original(rt, ctx);
    in_move = previous;
}
void goldeneye_input_process_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    observe(rt, ctx, "input");
    bool active = in_move && (uint32_t)ctx->gpr[31] == GE_INPUT_PROCESS_RETURN && goldeneye_input_playable(rt);
    if (active && gamepad_enabled && aim_stick.active && (!enabled || !mouse.pending.active)) {
        uint32_t p = player(rt), style = recomp_load32(rt, p + GE_INPUT_CONTROL_STYLE);
        // Native 2.2 Galore: primary stick looks; secondary stick walks/strafes.
        // Supply the second stick only at its checked joy calls, without making
        // a second player/controller visible to the menus or save system.
        move_x = (int8_t)ctx->gpr[4];
        move_y = (int8_t)ctx->gpr[5];
        aim_buttons = ((uint16_t)ctx->gpr[6] & 0x0010) ? 0x2000 : 0;
        recomp_store32(rt, p + GE_INPUT_CONTROL_STYLE, GE_INPUT_DUAL_STYLE);
        ctx->gpr[4] = mips_s32((uint32_t)(int32_t)lroundf(aim_stick.x * 80));
        // Native 2.2's positive Y looks down, matching SDL's downward Y.
        ctx->gpr[5] = mips_s32((uint32_t)(int32_t)lroundf(aim_stick.y * 80));
        bool previous = in_dual_input;
        in_dual_input = true;
        original(rt, ctx);
        in_dual_input = previous;
        recomp_store32(rt, p + GE_INPUT_CONTROL_STYLE, style);
        recomp_mouse_clear(&mouse);
        observe(rt, ctx, "gamepad_input");
        return;
    }
#ifdef RECOMP_DEBUG
    if (active && replay) {
        replay_updates++;
        if (next_move == replay_updates) {
            recomp_mouse_submit(&mouse, replay_frame);
            read_replay();
        }
    }
#endif
    if (!active || !enabled) {
        recomp_mouse_clear(&mouse);
        original(rt, ctx);
        return;
    }
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active) {
        original(rt, ctx);
        return;
    }
    uint32_t p = player(rt);
    float yaw = read_float(rt, p + GE_INPUT_YAW);
    float pitch = read_float(rt, p + GE_INPUT_PITCH);
    if (!isfinite(yaw) || !isfinite(pitch) || !isfinite(frame.dx) || !isfinite(frame.dy)) {
        recomp_mouse_clear(&mouse);
        original(rt, ctx);
        return;
    }
    yaw = (float)fmod(yaw + frame.dx * sensitivity, 360.0);
    if (yaw < 0)
        yaw += 360;
    pitch = (float)fmax(-90, fmin(90, pitch - frame.dy * sensitivity * vertical));
    write_float(rt, p + GE_INPUT_YAW, yaw);
    write_float(rt, p + GE_INPUT_PITCH, pitch);
    write_float(rt, p + GE_INPUT_YAW_SPEED, 0);
    write_float(rt, p + GE_INPUT_PITCH_SPEED, 0);
    // Recompute orientation before movement uses it, preserving guest registers.
    MipsContext scratch = *ctx;
    GE_INPUT_ORIGINAL_ANGLES(rt, &scratch);
    uint32_t style = recomp_load32(rt, p + GE_INPUT_CONTROL_STYLE);
    recomp_store32(rt, p + GE_INPUT_CONTROL_STYLE, 1); // 1.2 Solitaire, session-only during this call.
    ctx->gpr[4] = ctx->gpr[5] = 0;
    uint16_t buttons = (uint16_t)ctx->gpr[6];
    if (frame.move_x < 0)
        buttons |= 0x0002;
    if (frame.move_x > 0)
        buttons |= 0x0001;
    if (frame.move_y > 0)
        buttons |= 0x0008;
    if (frame.move_y < 0)
        buttons |= 0x0004;
    if (frame.wheel) {
        // Style 1.2: fresh A advances; A + fresh Z goes backwards. A suppresses
        // firing. Suppress B for this update to avoid the A+B remote-mine chord.
        buttons = (buttons & ~(0x8000 | 0x2000 | 0x4000)) | 0x8000;
        if (frame.wheel > 0)
            buttons |= 0x2000;
        ctx->gpr[7] = (uint16_t)ctx->gpr[7] & ~(0x8000 | 0x2000);
    }
    ctx->gpr[6] = buttons;
    observe(rt, ctx, "mouse_input");
    original(rt, ctx);
    recomp_store32(rt, p + GE_INPUT_CONTROL_STYLE, style);
    // The original still owns actions, collision and aim mode. Discard its
    // stick centering/turn rate while mouse owns the view, without dt scaling.
    if (goldeneye_input_playable(rt) && player(rt) == p) {
        write_float(rt, p + GE_INPUT_PITCH, pitch);
        write_float(rt, p + GE_INPUT_YAW_SPEED, 0);
        write_float(rt, p + GE_INPUT_PITCH_SPEED, 0);
    } else
        recomp_mouse_clear(&mouse);
    observe(rt, ctx, "mouse");
}
void goldeneye_input_stick_x_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool supply = in_dual_input && (uint32_t)ctx->gpr[31] == GE_INPUT_SECONDARY_X_RETURN;
    original(rt, ctx);
    if (supply)
        mips_set(ctx, 2, mips_s32((uint32_t)(int32_t)move_x));
}
void goldeneye_input_stick_y_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool supply = in_dual_input && (uint32_t)ctx->gpr[31] == GE_INPUT_SECONDARY_Y_RETURN;
    original(rt, ctx);
    if (supply)
        mips_set(ctx, 2, mips_s32((uint32_t)(int32_t)move_y));
}
void goldeneye_input_buttons_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool supply = in_dual_input && (uint32_t)ctx->gpr[31] == GE_INPUT_SECONDARY_BUTTONS_RETURN;
    uint16_t mask = (uint16_t)ctx->gpr[5];
    original(rt, ctx);
    if (supply)
        mips_set(ctx, 2, aim_buttons & mask);
}
void goldeneye_input_angles_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    observe(rt, ctx, "angles");
    original(rt, ctx);
}
